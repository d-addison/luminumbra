#pragma once

// ResidencyContract — the two-worlds determinism contract.
// PURPOSE
//   Formalize the SIMULATION-vs-RENDER residency split that today lives only as a
//   hand-maintained exclusion list (the render-mesh sub-hash is computed but omitted
//   from the run==replay match — main_server.cpp:469-478; ServerWorldRunner.cpp:527-533;
//   SystemConfig.cpp:223 skips render.* from the config sub-hash). This header declares
//   that partition as an explicit, named, self-checking contract.
//
// SCOPE / NEUTRALITY
//   This header is self-contained (only <cstdint>) and header-only. It is enforced in
//   production: WorldPersistenceRoundtrip.cpp derives the world_hash
//   exclusion scope from kChunkFieldResidency below (byte-neutral — the table encodes
//   the status-quo scope), and the activation queue implements the deterministic
//   availability contract.
//
//   The runtime registry is the authoritative partition. Named tag types remain available
//   for compile-time APIs without forcing a wrapper onto every hash-feeding signature.

#include <cstdint>

namespace luminumbra::core {

// ---------------------------------------------------------------------------------------
// Declared residency partition.
//
// Two residency classes, in one authoritative location:
//   * SimResidency    — DETERMINISTIC. Its state MAY contribute to world_hash.
//   * RenderResidency  — NONDETERMINISTIC. Its state MUST NOT contribute to world_hash.
//
// This 1:1 mirrors SystemConfig's `enum class Section { Sim, Render }`
// (SystemConfig.cpp:15): Section::Sim  <-> ResidencyClass::Sim  (may be hashed),
//                        Section::Render<-> ResidencyClass::Render (render.* never hashed,
//                        SystemConfig.cpp:223). Keeping the same rule here is the
//                        "config residency parity" of .
// ---------------------------------------------------------------------------------------
enum class ResidencyClass : std::uint8_t {
    Sim,    // deterministic; eligible to feed world_hash
    Render, // nondeterministic; forbidden from feeding world_hash
};

// Named tag types — the exact identifiers the spec's contract is checked against
// ( /  spell `SimResidency` and `RenderResidency`). Empty structs:
// they carry the class as a compile-time fact without imposing storage or wiring.
// `kClass` lets generic code recover the runtime enum from the tag type.
struct SimResidency {
    static constexpr ResidencyClass kClass = ResidencyClass::Sim;
};
struct RenderResidency {
    static constexpr ResidencyClass kClass = ResidencyClass::Render;
};

// ---------------------------------------------------------------------------------------
// Deterministic-input invariant (the predicate form).
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
// "Enforced, not remembered": the serialized-chunk-field
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
    // Water sim state. water residency (, derived-state reclassification, 2026-07-05): the
    // FIXED-POINT mm
    // arrays are the ONLY water sim truth; the float surface/flow mirrors and the
    // float terrain cache are one-way DERIVED render state (every writer now
    // regenerates them FROM mm — source injection, displacement, resize, and the
    // solver's own mirror update), so hashing them was double-counting derived
    // bytes and coupling the hash to float derivation. Reclassified Render as the
    // deliberate derived-state reclassification. max_water_delta_last_tick stays Sim: it is written
    // from
    // the integer mm delta and gates the sleep bookkeeping (evolution-relevant).
    {"water_level_data", ResidencyClass::Render},
    {"water_flow_data", ResidencyClass::Render},
    {"water_sim_terrain_height", ResidencyClass::Render},
    {"water_depth_mm", ResidencyClass::Sim},
    {"water_bed_mm", ResidencyClass::Sim},
    // / (authoritative-state change): flow momentum — evolution-relevant integer sim
    // truth, persisted + hashed (it was transient/cleared-on-load before, which made
    // the heavy oracle's resim leg diverge).
    {"water_edge_flux", ResidencyClass::Sim},
    {"has_water_sim", ResidencyClass::Sim},
    // Water bookkeeping.  resolved the parked question: water_mesh_generated
    // and water_mesh_dirty_ticks are MESHING bookkeeping mutated by the (worker-order-
    // dependent, render-side) mesh pipeline — the loaded-boot remesh flips them while
    // the water sim itself is paused, so hashing them makes the save/load water
    // round-trip impossible. Reclassified Render as the deliberate  bump
    // (authoritative-state change). The sleep/threshold fields stay Sim: they gate which chunks the
    // solver steps (evolution-relevant) and are only ever written by the solver.
    {"water_mesh_generated", ResidencyClass::Render},
    {"current_water_resolution", ResidencyClass::Sim},
    {"is_water_sleeping", ResidencyClass::Sim},
    {"max_water_delta_last_tick", ResidencyClass::Sim},
    {"ticks_below_threshold", ResidencyClass::Sim},
    {"water_mesh_dirty_ticks", ResidencyClass::Render},
    {"water_state", ResidencyClass::Sim},
};

} // namespace luminumbra::core
