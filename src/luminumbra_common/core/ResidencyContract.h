#pragma once

// ResidencyContract — the two-worlds determinism contract (Spec 018 FR-A + FR-B).
//
// Prose contract: docs/determinism-residency-contract.md
// Spec:           docs/specs/018-determinism-hardening/spec.md
//
// PURPOSE
//   Formalize the SIMULATION-vs-RENDER residency split that today lives only as a
//   hand-maintained exclusion list (the render-mesh sub-hash is computed but omitted
//   from the run==replay match — main_server.cpp:469-478; ServerWorldRunner.cpp:527-533;
//   SystemConfig.cpp:223 skips render.* from the config sub-hash). This header declares
//   that partition as an explicit, named, self-checking contract.
//
// SCOPE / NEUTRALITY
//   This header is SELF-CONTAINED (only <cstdint>) and header-only. Since SHIELD-06 it
//   is ENFORCED IN PRODUCTION: WorldPersistenceRoundtrip.cpp derives the world_hash
//   exclusion scope from kChunkFieldResidency below (byte-neutral — the table encodes
//   the status-quo scope), and Spec 017-B's activation queue implements the FR-B
//   availability contract.
//
//   OQ-1 (residency partition: compile-time type distinction vs runtime registry) is
//   left OPEN by Spec 018. This header declares the VOCABULARY (named tag types + a
//   runtime enum) so downstream code may key on either, but it deliberately does NOT
//   force a `Tagged<T, Class>` wrapper onto every hash-feeding signature — that invasive
//   choice is reserved for the consuming spec.

#include <cstdint>

namespace luminumbra::core {

// ---------------------------------------------------------------------------------------
// FR-A-001 — Declared residency partition.
//
// Two residency classes, in one authoritative location:
//   * SimResidency    — DETERMINISTIC. Its state MAY contribute to world_hash.
//   * RenderResidency  — NONDETERMINISTIC. Its state MUST NOT contribute to world_hash.
//
// This 1:1 mirrors SystemConfig's `enum class Section { Sim, Render }`
// (SystemConfig.cpp:15): Section::Sim  <-> ResidencyClass::Sim  (may be hashed),
//                        Section::Render<-> ResidencyClass::Render (render.* never hashed,
//                        SystemConfig.cpp:223). Keeping the same rule here is the
//                        "config residency parity" of FR-A-004.
// ---------------------------------------------------------------------------------------
enum class ResidencyClass : std::uint8_t {
    Sim,     // deterministic; eligible to feed world_hash
    Render,  // nondeterministic; forbidden from feeding world_hash
};

// Named tag types — the exact identifiers the spec's contract is checked against
// (FR-A-001 / AC-A-001 spell `SimResidency` and `RenderResidency`). Empty structs:
// they carry the class as a compile-time fact without imposing storage or wiring.
// `kClass` lets generic code recover the runtime enum from the tag type.
struct SimResidency {
    static constexpr ResidencyClass kClass = ResidencyClass::Sim;
};
struct RenderResidency {
    static constexpr ResidencyClass kClass = ResidencyClass::Render;
};

// ---------------------------------------------------------------------------------------
// FR-A-002 — Deterministic-input invariant (the predicate form).
//
// MayFeedWorldHash(c) is the single source of truth for "is this class allowed to
// contribute to ComputeWorldHash / ComputeWorldSubHashes?" Only SimResidency may.
// A RenderResidency value (render mesh, GPU readback, exposure, froxel jitter, …)
// must never be folded into the hash (ServerWorldRunner.cpp:527-533) nor required to
// match run==replay (main_server.cpp:469-478).
// ---------------------------------------------------------------------------------------
constexpr bool MayFeedWorldHash(ResidencyClass c) noexcept {
    return c == ResidencyClass::Sim;
}

// Self-checking presence assertions: the contract is wrong if these ever fail.
static_assert(MayFeedWorldHash(SimResidency::kClass),
              "SimResidency must be eligible to feed world_hash");
static_assert(!MayFeedWorldHash(RenderResidency::kClass),
              "RenderResidency must be forbidden from feeding world_hash");

// ---------------------------------------------------------------------------------------
// FR-B-001 — Deterministic availability set (the contract Spec 017-B consumes).
//
// A SimResidency value at a given sim tick must be derivable PURELY from the
// deterministic inputs below — never from wall-clock, job-completion order, thread
// scheduling, or GPU readback (FR-A-002). AvailabilityKey is exactly those inputs:
//
//   (seed, preset, deterministic-config, tick) + position
//
// matching the FR-A-002 enumeration. It is the stable, tick-keyed identity Spec 017-B's
// activation queue keys on when it replaces the main-thread wait_for_streaming_jobs()
// barrier (ServerWorldRunner.cpp:516/:546/:591) and the boot-settle of initial residency
// (ServerWorldRunner.cpp:369-393) with async ownership: 017 may change HOW residency
// settles, but the sim must still advance from this set — i.e. WHAT the sim sees per tick
// stays a pure function of AvailabilityKey.
//
//   config_sub_hash carries the deterministic, sim-only-when-enabled config sub-hash
//   (SystemConfig::ComputeConfigSubHash, SystemConfig.cpp:217-244): render.* is excluded
//   (:223) and an all-sim-default config hashes to empty (:243), so an unconfigured world
//   leaves this field as the empty-baseline sentinel (0) and the key is byte-stable.
// ---------------------------------------------------------------------------------------
struct AvailabilityKey {
    std::uint64_t seed = 0;            // world seed (deterministic)
    std::uint32_t preset_id = 0;       // worldgen preset identity (deterministic)
    std::uint64_t config_sub_hash = 0; // deterministic config sub-hash; 0 == empty baseline
    std::int64_t  tick = 0;            // sim tick index (deterministic clock)
    std::int32_t  chunk_x = 0;         // residency position (chunk coords)
    std::int32_t  chunk_y = 0;
    std::int32_t  chunk_z = 0;

    friend constexpr bool operator==(const AvailabilityKey& a,
                                     const AvailabilityKey& b) noexcept {
        return a.seed == b.seed && a.preset_id == b.preset_id &&
               a.config_sub_hash == b.config_sub_hash && a.tick == b.tick &&
               a.chunk_x == b.chunk_x && a.chunk_y == b.chunk_y && a.chunk_z == b.chunk_z;
    }
    friend constexpr bool operator!=(const AvailabilityKey& a,
                                     const AvailabilityKey& b) noexcept {
        return !(a == b);
    }
};

// FR-B-001/FR-B-002 — the AVAILABILITY SET contract, declared (not implemented).
//
//   The deterministic availability set at tick T is the set of AvailabilityKeys whose
//   residency has SETTLED for T (per-tick streaming barrier + boot-settle). Membership is
//   a PURE function of AvailabilityKey only:
//
//       IsResident(key)  ::  pure fn of AvailabilityKey
//                            — NEVER wall-clock, job-completion order, thread schedule,
//                              or GPU readback.
//
//   A deterministic system reads only keys for which IsResident(key) holds (FR-B-002:
//   since Spec 017-B landed, the ACTIVATION QUEUE — activate_due(tick) plus the explicit
//   force drains at boot/hash/mutate/teardown — is the only legal availability source;
//   the per-tick barrier it replaced was the original wording) and reads terrain through
//   the pure sampler, never shared mutable streaming buffers (FR-B-004). Membership is
//   implemented by the consuming system / Spec 017-B; this header only fixes the
//   contract's SHAPE, so it stays behavior-neutral. The signature alias documents it:
using IsResidentFn = bool (*)(const AvailabilityKey& key);

// ---------------------------------------------------------------------------------------
// FR-A-003 — "Enforced, not remembered" (SHIELD-06): the serialized-chunk-field
// residency table.
//
// Every field ChunkToJson serializes is classified here, in the contract's one
// authoritative location. The persistence hash-exclusion scope is DERIVED from this
// table (WorldPersistenceRoundtrip.cpp — the first production consumer of this header):
// a field feeds world_hash iff MayFeedWorldHash(its class). The hash path verifies at
// first use that every serialized key is classified, so adding a chunk field without
// classifying it fails LOUDLY instead of silently joining (or silently escaping) the
// hash. The ResidencyContract.HashScopeDerivesFromPartition gtest pins the projection.
//
// Classification is the STATUS QUO hash scope (byte-neutral by construction). Fields
// whose Sim classification is under review are annotated — reclassifying any of them is
// a deliberate world_hash bump, never a side effect of this table.
// ---------------------------------------------------------------------------------------
struct ChunkFieldResidency {
    const char* field;
    ResidencyClass residency;
};
inline constexpr ChunkFieldResidency kChunkFieldResidency[] = {
    // Identity + lifecycle (state/state_value feed the terrain sub-hash).
    {"coords", ResidencyClass::Sim},
    {"chunk_id", ResidencyClass::Sim},
    {"state", ResidencyClass::Sim},
    {"state_value", ResidencyClass::Sim},
    // Voxel sim truth.
    {"sdf_data", ResidencyClass::Sim},
    {"heightmap_data", ResidencyClass::Sim},
    {"material_data", ResidencyClass::Sim},
    // Render meshes + meshing bookkeeping (worker-order-dependent bytes; the
    // historical kRenderMeshHashExcludedFields set, verbatim).
    {"mesh_vertices", ResidencyClass::Render},
    {"mesh_indices", ResidencyClass::Render},
    {"water_mesh_vertices", ResidencyClass::Render},
    {"water_mesh_indices", ResidencyClass::Render},
    {"pending_mesh_vertices", ResidencyClass::Render},
    {"pending_mesh_indices", ResidencyClass::Render},
    {"pending_water_mesh_vertices", ResidencyClass::Render},
    {"pending_water_mesh_indices", ResidencyClass::Render},
    {"mesh_version", ResidencyClass::Render},
    {"water_mesh_version", ResidencyClass::Render},
    {"pending_mesh_ready", ResidencyClass::Render},
    {"pending_mesh_failed", ResidencyClass::Render},
    {"current_lod", ResidencyClass::Render},
    {"pending_lod", ResidencyClass::Render},
    // Collision (built from the heightmap; hashed).
    {"has_collision", ResidencyClass::Sim},
    // Water sim state (spec 009 authoritative fixed-point + mirrors).
    {"water_level_data", ResidencyClass::Sim},
    {"water_flow_data", ResidencyClass::Sim},
    {"water_sim_terrain_height", ResidencyClass::Sim},
    {"water_depth_mm", ResidencyClass::Sim},
    {"water_bed_mm", ResidencyClass::Sim},
    {"has_water_sim", ResidencyClass::Sim},
    // Status-quo-hashed water bookkeeping, Sim BY PRECEDENT (they are in the
    // hash today). water_mesh_generated / water_mesh_dirty_ticks smell
    // render-adjacent and the water section's persistence/settle design is
    // under WATER-17 — any reclassification is a deliberate bump there.
    {"water_mesh_generated", ResidencyClass::Sim},
    {"current_water_resolution", ResidencyClass::Sim},
    {"is_water_sleeping", ResidencyClass::Sim},
    {"max_water_delta_last_tick", ResidencyClass::Sim},
    {"ticks_below_threshold", ResidencyClass::Sim},
    {"water_mesh_dirty_ticks", ResidencyClass::Sim},
    {"water_state", ResidencyClass::Sim},
};

}  // namespace luminumbra::core
