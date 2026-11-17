#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/GroundObjectStore.h"

namespace Luminumbra::Server {

// Per-capture hash set used by the heavy-mode oracle (server/modes/Heavy.cpp).
struct HeavyHashes {
    std::string world_hash;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    std::string scent_hash;
    std::vector<std::uint64_t> scent_bits;
    bool active_regions = false;
    std::string clock_bytes;
    std::string ledger_bytes;
    // All durable object fields, in ascending persistent-id order. Compare independently
    // of world_hash: the composite also includes derived mesh state and is exempt below.
    std::vector<world::GroundObject> ground_objects;
    std::string ground_notifications;
};

inline std::vector<std::uint64_t> CaptureScentBits(const luminumbra::ai::ScentField& field) {
    std::vector<std::uint64_t> bits;
    for (int ch = 0; ch < field.channels(); ++ch) {
        for (int z = 0; z < field.height(); ++z) {
            for (int x = 0; x < field.width(); ++x) {
                bits.push_back(std::bit_cast<std::uint64_t>(field.Sample(ch, x, z)));
            }
        }
    }
    return bits;
}

inline std::string ScentDifference(const HeavyHashes& a, const HeavyHashes& b) {
    if (a.active_regions || b.active_regions) {
        if (a.scent_bits != b.scent_bits) {
            return "scent_grid";
        }
        if (a.scent_hash != b.scent_hash) {
            return "scents";
        }
    }
    return {};
}

inline std::vector<world::GroundObject>
CaptureGroundObjects(const world::GroundObjectStore& store) {
    std::vector<world::GroundObject> objects;
    objects.reserve(store.size());
    for (const auto& [id, object] : store.objects()) {
        objects.push_back(object);
    }
    return objects;
}

// Empty on equality; otherwise identify the first differing durable field in canonical order.
inline std::string GroundObjectDifference(const HeavyHashes& a, const HeavyHashes& b) {
    if (a.ground_objects.size() != b.ground_objects.size()) {
        return "ground_objects.size";
    }
    for (std::size_t i = 0; i < a.ground_objects.size(); ++i) {
        const auto& lhs = a.ground_objects[i];
        const auto& rhs = b.ground_objects[i];
        if (lhs == rhs) {
            continue;
        }
        const std::array<std::pair<const char*, bool>, 10> fields{{
            {"id", lhs.id == rhs.id},
            {"type", lhs.type == rhs.type},
            {"x_mm", lhs.x_mm == rhs.x_mm},
            {"y_mm", lhs.y_mm == rhs.y_mm},
            {"z_mm", lhs.z_mm == rhs.z_mm},
            {"spawn_tick", lhs.spawn_tick == rhs.spawn_tick},
            {"active_age", lhs.active_age == rhs.active_age},
            {"despawn_after", lhs.despawn_after == rhs.despawn_after},
            {"payload", lhs.payload == rhs.payload},
            {"flags", lhs.flags == rhs.flags},
        }};
        for (const auto& [name, equal] : fields) {
            if (!equal) {
                return "ground_objects[id=" + std::to_string(lhs.id) + "]." + name;
            }
        }
    }
    return {};
}

// One row per field of HeavyHashes. `compared` rows must match across the
// round-trip and the resim; `exempt_reason` documents why an uncompared row is
// excluded (non-null exactly when `compared` is false). `requires_active_regions`
// rows are only compared when the persisted clock/ambient layer is active on at
// least one side. `mutate` changes exactly that field; it exists so the
// comparison table can be exercised by tests.
//
// Why mesh is excluded from the heavy oracle's equality: mesh is a
// deterministically-regenerated DERIVED render artifact. The async re-meshing of
// chunks adopted across a save/load boundary legitimately reaches the same
// geometry via a different in-memory pending-mesh/version snapshot than the
// originating session held. The sub-hashes localize this precisely (only `mesh`
// differs; terrain/water/entities are byte-identical), which is exactly the
// desync-localization the sub-hashes exist to provide. The top-level world_hash
// (which DOES include mesh) is reported but not asserted on across the
// round-trip for this reason; it is still asserted run==replay in --smoke.
struct HeavySubsystem {
    const char* name;
    bool compared;
    bool requires_active_regions;
    const char* exempt_reason;
    void (*mutate)(HeavyHashes&);
};

inline std::span<const HeavySubsystem> HeavySubsystemTable() {
    static const std::array<HeavySubsystem, 16> kTable{{
        {"terrain",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.terrain += "x";
         }},
        {"water",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.water += "x";
         }},
        {"entities",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.entities += "x";
         }},
        {"ground_objects",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             if (h.ground_objects.empty()) {
                 h.ground_objects.push_back(world::GroundObject{.id = 1, .type = 1});
             } else {
                 h.ground_objects.clear();
             }
         }},
        {"active_regions",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.active_regions = !h.active_regions;
         }},
        {"ground_notifications",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.ground_notifications += "x";
         }},
        {"clock",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.clock_bytes += "x";
         }},
        {"ledger",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.ledger_bytes += "x";
         }},
        {"wind",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.wind += "x";
         }},
        {"weather",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.weather += "x";
         }},
        {"aether",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.aether += "x";
         }},
        {"aether_state",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.aether_state += "x";
         }},
        {"mesh",
         false,
         false,
         "derived render artifact; async re-meshing across a save/load boundary reaches the "
         "same geometry via a different pending-mesh snapshot",
         [](HeavyHashes& h) {
             h.sub.mesh += "x";
         }},
        {"scents",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.scent_hash += "x";
         }},
        {"scent_grid",
         true,
         true,
         nullptr,
         [](HeavyHashes& h) {
             h.scent_bits.push_back(1);
         }},
        {"world_hash",
         false,
         false,
         "composite includes the mesh sub-hash; asserted run==replay in --smoke instead",
         [](HeavyHashes& h) {
             h.world_hash += "x";
         }},
    }};
    return kTable;
}

// Equality over AUTHORITATIVE SIMULATION STATE (terrain SDF + water sim +
// entities + ground objects). Legacy loads restart at tick zero. With the persisted clock on,
// ambient fields, scent and the clock must also agree at save and after continuation.
inline bool AuthoritativeStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    if (a.active_regions || b.active_regions) {
        if (a.active_regions != b.active_regions || a.clock_bytes != b.clock_bytes ||
            a.ledger_bytes != b.ledger_bytes || a.sub.wind != b.sub.wind ||
            a.sub.weather != b.sub.weather || a.sub.aether != b.sub.aether ||
            a.sub.aether_state != b.sub.aether_state || a.scent_hash != b.scent_hash ||
            a.scent_bits != b.scent_bits) {
            return false;
        }
    }
    // The water checkpoint includes the nonzero scheduling cursor even when
    // active regions are disabled; it affects which awake chunks advance next.
    return a.sub.terrain == b.sub.terrain && a.sub.water == b.sub.water &&
           a.sub.entities == b.sub.entities && a.ground_objects == b.ground_objects &&
           a.ground_notifications == b.ground_notifications;
}

inline bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

// Separate explicit-capture oracle. None of these fields widen HeavyHashes or its
// legacy exclusions. Only participating owners/ledger records are copied, at the
// four Heavy checkpoints, never on a simulation tick. Existing admission bounds
// are 1 MiB aggregate owner payload and ActiveRegionLedger::kMaxRegions records.
struct HeavyOwnerScheduler {
    bool active_regions = false;
    world::WorldClock clock;
    std::uint64_t ledger_tick = 0;
    std::uint64_t ledger_header_tick = 0;
    std::uint64_t next_persistent_id = 1;
    std::uint64_t water_cursor = 0;
    world::RegionSchedulerConfig config;
    std::optional<Vec3> local_anchor;
    std::map<world::RegionKey, world::ActiveRegionRecord> records;
    world::ActiveRegionLedger::EnergyAges energy_ages;
    world::ActiveRegionLedger::WaterDebts water_debts;
    std::map<world::RegionKey, std::uint64_t> completed_work;
};

struct HeavyOwnerCapture {
    bool captured = false;
    Persistence::OwnerContinuation owners;
    HeavyOwnerScheduler scheduler;
};

inline HeavyOwnerScheduler CaptureHeavyOwnerScheduler(bool active_regions,
                                                      const world::WorldClock& clock,
                                                      const world::ActiveRegionLedger& ledger,
                                                      std::uint64_t water_cursor) {
    return {.active_regions = active_regions,
            .clock = clock,
            .ledger_tick = ledger.tick(),
            .ledger_header_tick = ledger.header_tick(),
            .next_persistent_id = ledger.next_persistent_id(),
            .water_cursor = water_cursor,
            .config = ledger.config(),
            .local_anchor = ledger.local_anchor(),
            .records = ledger.records(),
            .energy_ages = ledger.energy_ages(),
            .water_debts = ledger.water_debts(),
            .completed_work = ledger.completed_work()};
}

// Named exact scalar projections for diagnostics. Floats are always integer bits:
// signed zero, subnormals and even diagnostic non-finite values remain distinct.
struct HeavyOwnerField {
    const char* name;
    std::uint64_t bits;
};
inline std::uint32_t HeavyOwnerFloatBits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

inline std::array<HeavyOwnerField, 10> HeavyOwnerFields(const Persistence::OwnerContinuation& v) {
    return {{
        {"simulation_tick", v.simulation_tick},
        {"stream_tick", v.stream_tick},
        {"moving_anchor", v.moving_anchor},
        {"water_smoke", v.water_smoke},
        {"origin.x", HeavyOwnerFloatBits(v.origin.x)},
        {"origin.y", HeavyOwnerFloatBits(v.origin.y)},
        {"origin.z", HeavyOwnerFloatBits(v.origin.z)},
        {"anchor.x", HeavyOwnerFloatBits(v.anchor.x)},
        {"anchor.y", HeavyOwnerFloatBits(v.anchor.y)},
        {"anchor.z", HeavyOwnerFloatBits(v.anchor.z)},
    }};
}

inline std::array<HeavyOwnerField, 8> HeavyOwnerFields(const World::PlayerAvatar& v) {
    return {{
        {"player_id", v.player_id},
        {"position.x", HeavyOwnerFloatBits(v.position.x)},
        {"position.y", HeavyOwnerFloatBits(v.position.y)},
        {"position.z", HeavyOwnerFloatBits(v.position.z)},
        {"facing", HeavyOwnerFloatBits(v.facing)},
        {"velocity.x", HeavyOwnerFloatBits(v.velocity.x)},
        {"velocity.y", HeavyOwnerFloatBits(v.velocity.y)},
        {"velocity.z", HeavyOwnerFloatBits(v.velocity.z)},
    }};
}

inline std::array<HeavyOwnerField, 4>
HeavyOwnerFields(const Persistence::PhysicsOwnerContinuation& v) {
    return {{
        {"owner_id", v.owner_id},
        {"anchor.x", HeavyOwnerFloatBits(v.anchor.x)},
        {"anchor.y", HeavyOwnerFloatBits(v.anchor.y)},
        {"anchor.z", HeavyOwnerFloatBits(v.anchor.z)},
    }};
}

inline std::array<HeavyOwnerField, 2>
HeavyOwnerFields(const Persistence::TerrainOwnerContinuation& v) {
    return {{
        {"chunk_id", v.chunk_id},
        {"body_id", v.body_id},
    }};
}

inline std::array<HeavyOwnerField, 20> HeavyOwnerFields(const world::ActiveRegionRecord& v) {
    return {{
        {"visited", v.visited},
        {"edited", v.edited},
        {"pinned", v.pinned},
        {"populated", v.populated},
        {"first_active", v.first_active},
        {"last_proximity", v.last_proximity},
        {"last_edit", v.last_edit},
        {"last_ticked", v.last_ticked},
        {"frozen_at", v.frozen_at},
        {"last_save", v.last_save},
        {"state", static_cast<std::uint64_t>(v.state)},
        {"cadence_shift", v.cadence_shift},
        {"cadence_phase", v.cadence_phase},
        {"pressure", v.pressure},
        {"over_hold", v.over_hold},
        {"under_hold", v.under_hold},
        {"last_decision", v.last_decision},
        {"water_cursor", v.water_cursor},
        {"frozen_digest", v.frozen_digest},
        {"wake_pending", v.wake_pending},
    }};
}

inline std::array<HeavyOwnerField, 3> HeavyOwnerFields(const world::RegionSchedulerConfig& v) {
    return {{
        {"work_limit", v.work_limit},
        {"hold_ticks", v.hold_ticks},
        {"reduced_shift", v.reduced_shift},
    }};
}

inline std::array<HeavyOwnerField, 5> HeavyOwnerFields(const world::WorldCalendar& v) {
    return {{
        {"dayLengthTicks", v.dayLengthTicks},
        {"daysPerYear", v.daysPerYear},
        {"daylightNumerator", v.daylightNumerator},
        {"daylightDenominator", v.daylightDenominator},
        {"lunarCycleDays", v.lunarCycleDays},
    }};
}

inline std::array<HeavyOwnerField, 5> HeavyOwnerFields(const HeavyOwnerScheduler& v) {
    return {{
        {"active_regions", v.active_regions},
        {"ledger_tick", v.ledger_tick},
        {"ledger_header_tick", v.ledger_header_tick},
        {"next_persistent_id", v.next_persistent_id},
        {"water_cursor", v.water_cursor},
    }};
}

template<std::size_t N>
inline std::string HeavyOwnerScalarDifference(const std::string& prefix,
                                              const std::array<HeavyOwnerField, N>& a,
                                              const std::array<HeavyOwnerField, N>& b) {
    for (std::size_t i = 0; i < N; ++i) {
        if (a[i].bits != b[i].bits) {
            return prefix + "." + a[i].name;
        }
    }
    return {};
}

inline std::string
HeavyOwnerVectorDifference(const std::string& prefix, const Vec3& a, const Vec3& b) {
    constexpr std::array<const char*, 3> axes{"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        if (HeavyOwnerFloatBits(a[axis]) != HeavyOwnerFloatBits(b[axis])) {
            return prefix + "." + axes[static_cast<std::size_t>(axis)];
        }
    }
    return {};
}

inline std::string HeavyPhysicsOwnerDifference(const std::string& prefix,
                                               const World::PhysicsOwnerState& a,
                                               const World::PhysicsOwnerState& b) {
    // Compare the canonical owner encoding, never object padding or unused capacity.
    // Byte indices are stable field names even for variable-length contact schemas.
    if (a.size > a.bytes.size() || b.size > b.bytes.size()) {
        return prefix + ".invalid_size";
    }
    if (a.size != b.size) {
        return prefix + ".size";
    }
    for (std::size_t i = 0; i < a.size; ++i) {
        if (a.bytes[i] != b.bytes[i]) {
            return prefix + ".bytes[" + std::to_string(i) + "]";
        }
    }
    return {};
}

template<class T, class Compare>
inline std::string HeavyOwnerSequenceDifference(const std::string& prefix,
                                                const std::vector<T>& a,
                                                const std::vector<T>& b,
                                                Compare compare) {
    if (a.size() != b.size()) {
        return prefix + ".size";
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto difference = compare(prefix + "[" + std::to_string(i) + "]", a[i], b[i]);
        if (!difference.empty()) {
            return difference;
        }
    }
    return {};
}

inline std::string HeavyBodyOwnerDifference(const std::string& prefix,
                                            const Persistence::PhysicsOwnerContinuation& a,
                                            const Persistence::PhysicsOwnerContinuation& b) {
    const auto descriptor =
        HeavyOwnerScalarDifference(prefix, HeavyOwnerFields(a), HeavyOwnerFields(b));
    return descriptor.empty() ? HeavyPhysicsOwnerDifference(prefix + ".physics", a.state, b.state)
                              : descriptor;
}

inline std::string HeavyOwnerContinuationDifference(const Persistence::OwnerContinuation& a,
                                                    const Persistence::OwnerContinuation& b) {
    auto difference = HeavyOwnerScalarDifference("owner", HeavyOwnerFields(a), HeavyOwnerFields(b));
    if (!difference.empty()) {
        return difference;
    }
    difference = HeavyOwnerSequenceDifference(
        "owner.avatars",
        a.avatars,
        b.avatars,
        [](const auto& prefix, const auto& lhs, const auto& rhs) {
            const auto avatar = HeavyOwnerScalarDifference(
                prefix + ".avatar", HeavyOwnerFields(lhs.avatar), HeavyOwnerFields(rhs.avatar));
            return avatar.empty()
                       ? HeavyPhysicsOwnerDifference(prefix + ".physics", lhs.physics, rhs.physics)
                       : avatar;
        });
    if (!difference.empty()) {
        return difference;
    }
    difference =
        HeavyOwnerSequenceDifference("owner.bodies", a.bodies, b.bodies, HeavyBodyOwnerDifference);
    if (!difference.empty()) {
        return difference;
    }
    if (a.player.has_value() != b.player.has_value()) {
        return "owner.player.present";
    }
    if (a.player && b.player) {
        difference = HeavyBodyOwnerDifference("owner.player", *a.player, *b.player);
        if (!difference.empty()) {
            return difference;
        }
    }
    return HeavyOwnerSequenceDifference(
        "owner.terrain",
        a.terrain,
        b.terrain,
        [](const auto& prefix, const auto& lhs, const auto& rhs) {
            return HeavyOwnerScalarDifference(prefix, HeavyOwnerFields(lhs), HeavyOwnerFields(rhs));
        });
}

template<class T>
inline std::string HeavyLedgerMapDifference(const std::string& prefix,
                                            const std::map<world::RegionKey, T>& a,
                                            const std::map<world::RegionKey, T>& b) {
    if (a.size() != b.size()) {
        return prefix + ".size";
    }
    auto rhs = b.begin();
    for (const auto& [key, value] : a) {
        const auto path = prefix + "[" + std::to_string(key.x) + "," + std::to_string(key.z) + "]";
        if (key.x != rhs->first.x) {
            return path + ".key.x";
        }
        if (key.z != rhs->first.z) {
            return path + ".key.z";
        }
        if constexpr (std::is_same_v<T, world::ActiveRegionRecord>) {
            const auto difference = HeavyOwnerScalarDifference(
                path, HeavyOwnerFields(value), HeavyOwnerFields(rhs->second));
            if (!difference.empty()) {
                return difference;
            }
        } else if (value != rhs->second) {
            return path + ".value";
        }
        ++rhs;
    }
    return {};
}

inline std::string HeavyOwnerSchedulerDifference(const HeavyOwnerScheduler& a,
                                                 const HeavyOwnerScheduler& b) {
    auto difference =
        HeavyOwnerScalarDifference("scheduler", HeavyOwnerFields(a), HeavyOwnerFields(b));
    if (!difference.empty()) {
        return difference;
    }
    if (a.clock.tick() != b.clock.tick()) {
        return "scheduler.clock.tick";
    }
    difference = HeavyOwnerScalarDifference("scheduler.clock.calendar",
                                            HeavyOwnerFields(a.clock.calendar()),
                                            HeavyOwnerFields(b.clock.calendar()));
    if (!difference.empty()) {
        return difference;
    }
    difference = HeavyOwnerScalarDifference(
        "scheduler.config", HeavyOwnerFields(a.config), HeavyOwnerFields(b.config));
    if (!difference.empty()) {
        return difference;
    }
    if (a.local_anchor.has_value() != b.local_anchor.has_value()) {
        return "scheduler.local_anchor.present";
    }
    if (a.local_anchor && b.local_anchor) {
        difference =
            HeavyOwnerVectorDifference("scheduler.local_anchor", *a.local_anchor, *b.local_anchor);
        if (!difference.empty()) {
            return difference;
        }
    }
    difference = HeavyLedgerMapDifference("scheduler.records", a.records, b.records);
    if (!difference.empty()) {
        return difference;
    }
    difference = HeavyLedgerMapDifference("scheduler.energy_ages", a.energy_ages, b.energy_ages);
    if (!difference.empty()) {
        return difference;
    }
    difference = HeavyLedgerMapDifference("scheduler.water_debts", a.water_debts, b.water_debts);
    return difference.empty() ? HeavyLedgerMapDifference(
                                    "scheduler.completed_work", a.completed_work, b.completed_work)
                              : difference;
}

// This is also the Heavy profile gate. Disabled comparisons accept even refused
// captures, leaving the legacy oracle's decisions and diagnostics untouched.
inline std::string HeavyOwnerDifference(bool owner_continuation,
                                        const HeavyOwnerCapture& a,
                                        const HeavyOwnerCapture& b) {
    if (!owner_continuation) {
        return {};
    }
    if (!a.captured || !b.captured) {
        return "owner.capture_refused";
    }
    const auto difference = HeavyOwnerContinuationDifference(a.owners, b.owners);
    return difference.empty() ? HeavyOwnerSchedulerDifference(a.scheduler, b.scheduler)
                              : difference;
}

} // namespace Luminumbra::Server
