#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"

namespace Luminumbra::Server {

// Per-capture hash set used by the heavy-mode oracle (server/modes/Heavy.cpp).
struct HeavyHashes {
    std::string world_hash;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    std::string scent_hash;
    bool active_regions = false;
    std::string clock_bytes;
    std::string ledger_bytes;
    std::uint64_t water_scheduler_cursor = 0;
};

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
        {"water_scheduler_cursor",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.water_scheduler_cursor ^= 1u;
         }},
        {"entities",
         true,
         false,
         nullptr,
         [](HeavyHashes& h) {
             h.sub.entities += "x";
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
         false,
         false,
         "persisted scent state is reported but not yet asserted by the heavy oracle; the "
         "activation-slice oracle owns its comparison and negative control",
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
// entities). Legacy loads restart at tick zero. With the persisted clock on, ambient fields and the
// clock must also agree at save and after continuation.
inline bool AuthoritativeStateEqual(const HeavyHashes& a, const HeavyHashes& b) {
    if (a.active_regions || b.active_regions) {
        if (a.active_regions != b.active_regions || a.clock_bytes != b.clock_bytes ||
            a.ledger_bytes != b.ledger_bytes || a.sub.wind != b.sub.wind ||
            a.sub.weather != b.sub.weather || a.sub.aether != b.sub.aether ||
            a.sub.aether_state != b.sub.aether_state) {
            return false;
        }
    }
    return a.water_scheduler_cursor == b.water_scheduler_cursor && a.sub.terrain == b.sub.terrain &&
           a.sub.water == b.sub.water && a.sub.entities == b.sub.entities;
}

inline bool MeshEqual(const HeavyHashes& a, const HeavyHashes& b) {
    return a.sub.mesh == b.sub.mesh;
}

} // namespace Luminumbra::Server
