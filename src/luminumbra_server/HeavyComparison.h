#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/GroundObjectStore.h"

namespace Luminumbra::Server {

// Per-capture hash set used by the heavy-mode oracle (server/modes/Heavy.cpp).
struct HeavyHashes {
    std::string world_hash;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    std::string scent_hash;
    bool active_regions = false;
    std::string clock_bytes;
    std::string ledger_bytes;
    // All durable object fields, in ascending persistent-id order. Compare independently
    // of world_hash: the composite also includes derived mesh state and is exempt below.
    std::vector<world::GroundObject> ground_objects;
};

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

inline std::string ScentDifference(const HeavyHashes& a, const HeavyHashes& b) {
    if ((a.active_regions || b.active_regions) && a.scent_hash != b.scent_hash) {
        return "scents";
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
    static const std::array<HeavySubsystem, 14> kTable{{
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
// ambient fields, durable regional scent and the clock must also agree at save and after
// continuation. Without active regions, scent is a transient field that is not persisted,
// so its hash remains excluded from round-trip and resim equality.
inline bool AuthoritativeStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    if (a.active_regions || b.active_regions) {
        if (a.active_regions != b.active_regions || a.clock_bytes != b.clock_bytes ||
            a.ledger_bytes != b.ledger_bytes || a.sub.wind != b.sub.wind ||
            a.sub.weather != b.sub.weather || a.sub.aether != b.sub.aether ||
            a.sub.aether_state != b.sub.aether_state || a.scent_hash != b.scent_hash) {
            return false;
        }
    }
    return a.sub.terrain == b.sub.terrain && a.sub.water == b.sub.water &&
           a.sub.entities == b.sub.entities && a.ground_objects == b.ground_objects;
}

inline bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

} // namespace Luminumbra::Server
