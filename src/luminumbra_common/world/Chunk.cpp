#include "Chunk.h"
#include "../core/Log.h"
#include <algorithm>
#include <cmath>

namespace Luminumbra {

Chunk::Chunk(const IVec3& coords)
    : m_coords(coords)
    , m_id(calculate_id(coords))
    , m_state(ChunkState::Unloaded) {}

bool Chunk::has_complete_water_state() const {
    const auto& chunk = *this;
    const int resolution = chunk.current_water_resolution.load();
    if (!chunk.has_water_sim.load() ||
        (resolution != 4 && resolution != 8 && resolution != 16 && resolution != 32))
        return false;
    // Validated before multiplication: at most 1,024 cells / 2,048 flux entries.
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    if (chunk.water_depth_mm.size() != cells || chunk.water_bed_mm.size() != cells ||
        chunk.water_edge_flux.size() != 2u * cells || chunk.ticks_below_threshold < 0 ||
        !std::isfinite(chunk.max_water_delta_last_tick))
        return false;
    if (chunk.water_level_data.size() != cells || chunk.water_flow_data.size() != cells ||
        chunk.water_sim_terrain_height.size() != cells)
        return false;
    for (const auto* mirror :
         {&chunk.water_level_data, &chunk.water_sim_terrain_height, &chunk.water_rest_level}) {
        if ((!mirror->empty() && mirror->size() != cells) ||
            !std::all_of(
                mirror->begin(), mirror->end(), [](float value) { return std::isfinite(value); }))
            return false;
    }
    if ((!chunk.water_flow_data.empty() && chunk.water_flow_data.size() != cells) ||
        !std::all_of(
            chunk.water_flow_data.begin(), chunk.water_flow_data.end(), [](const Vec2& value) {
                return std::isfinite(value.x) && std::isfinite(value.y);
            }))
        return false;
    return std::all_of(chunk.water_depth_mm.begin(),
                       chunk.water_depth_mm.end(),
                       [](std::int32_t depth) { return depth >= 0; });
}

bool Chunk::same_water_state(const Chunk& rhs) const {
    const auto& lhs = *this;
    // Legacy containers have no per-layer water revision or precedence. Only
    // equal evolution-relevant state can represent duplicate copies of a column.
    // Persisted mirrors gate queries/sources and resizing, so ambiguous mirrors
    // also refuse migration. Render counters and source masks are accelerators.
    return lhs.water_depth_mm == rhs.water_depth_mm && lhs.water_bed_mm == rhs.water_bed_mm &&
           lhs.water_edge_flux == rhs.water_edge_flux &&
           lhs.water_level_data == rhs.water_level_data &&
           lhs.water_flow_data == rhs.water_flow_data &&
           lhs.water_sim_terrain_height == rhs.water_sim_terrain_height &&
           lhs.current_water_resolution.load() == rhs.current_water_resolution.load() &&
           lhs.has_water_sim.load() == rhs.has_water_sim.load() &&
           lhs.is_water_sleeping.load() == rhs.is_water_sleeping.load() &&
           lhs.ticks_below_threshold == rhs.ticks_below_threshold &&
           std::bit_cast<u32>(lhs.max_water_delta_last_tick) ==
               std::bit_cast<u32>(rhs.max_water_delta_last_tick);
}

void Chunk::clear_water_data() {
    water_level_data.clear();
    water_flow_data.clear();
    water_sim_terrain_height.clear();
    water_rest_level.clear();
    water_depth_mm.clear();
    water_bed_mm.clear();
    water_edge_flux.clear();
    water_src_mm.clear();
    water_mesh_vertices.clear();
    water_mesh_indices.clear();
    has_water_sim.store(false, std::memory_order_release);
    water_mesh_generated.store(false, std::memory_order_release);
    current_water_resolution.store(8, std::memory_order_release);
    is_water_sleeping.store(false, std::memory_order_release);
    max_water_delta_last_tick = 0.0f;
    ticks_below_threshold = 0;
    water_mesh_dirty_ticks = 0;
    m_water_authority.store(false, std::memory_order_release);
    m_hash_revision.fetch_add(1u, std::memory_order_acq_rel);
    m_voxel_data_dirty.store(true, std::memory_order_release);
}

bool Chunk::is_valid_state_transition(ChunkState from, ChunkState to) {
    if (from == to) {
        return true;
    }

    switch (from) {
        case ChunkState::Unloaded:
            return to == ChunkState::Loading;
        case ChunkState::Loading:
            return to == ChunkState::Idle || to == ChunkState::Unloading;
        case ChunkState::Idle:
            return to == ChunkState::Meshing || to == ChunkState::Unloading;
        case ChunkState::Meshing:
            return to == ChunkState::Ready || to == ChunkState::Unloading;
        case ChunkState::Ready:
            return to == ChunkState::Meshing || to == ChunkState::Unloading;
        case ChunkState::Unloading:
            return to == ChunkState::Unloaded;
    }

    return false;
}

bool Chunk::try_set_state(ChunkState expected_state, ChunkState new_state) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (m_state != expected_state || !is_valid_state_transition(m_state, new_state)) {
        return false;
    }

    m_state = new_state;
    return true;
}

ChunkID Chunk::calculate_id(const IVec3& coords) {
    // A robust, collision-free method for generating a unique ID from 3D integer coordinates
    // by packing the 32-bit integers into a single 64-bit ID. This method guarantees
    // uniqueness as long as the coordinates fit within the allocated bit-space.
    //
    // We allocate 21 bits for X, 21 bits for Z, and 22 bits for Y. This allows
    // coordinates in a massive range, roughly:
    // X/Z: [-1,048,576 to +1,048,575]
    // Y:   [-2,097,152 to +2,097,151]

    // Define masks for the number of bits allocated to each component.
    constexpr u64 X_BITS = 21;
    constexpr u64 Z_BITS = 21;
    // Y gets the remaining bits: 64 - 21 - 21 = 22
    constexpr u64 Y_BITS = 22;

    constexpr u64 X_MASK = (1ULL << X_BITS) - 1;
    constexpr u64 Y_MASK = (1ULL << Y_BITS) - 1;
    constexpr u64 Z_MASK = (1ULL << Z_BITS) - 1;

    // Cast signed integers to unsigned for correct bitwise operations.
    // The bit masks will handle negative numbers correctly via two's complement behavior.
    u64 x_packed = static_cast<u64>(coords.x) & X_MASK;
    u64 y_packed = static_cast<u64>(coords.y) & Y_MASK;
    u64 z_packed = static_cast<u64>(coords.z) & Z_MASK;

    // Pack the components into the 64-bit integer.
    // The order and shifts ensure no overlap.
    return (y_packed << (X_BITS + Z_BITS)) | (z_packed << X_BITS) | x_packed;
}

IVec3 Chunk::decode_id(ChunkID id) {
    constexpr u64 X_BITS = 21;
    constexpr u64 Z_BITS = 21;
    constexpr u64 Y_BITS = 22;
    constexpr u64 X_MASK = (1ULL << X_BITS) - 1;
    constexpr u64 Z_MASK = (1ULL << Z_BITS) - 1;
    constexpr u64 Y_MASK = (1ULL << Y_BITS) - 1;
    const auto sign_extend = [](u64 value, u64 bits) -> i32 {
        const u64 sign = 1ULL << (bits - 1u);
        return static_cast<i32>(static_cast<i64>((value ^ sign) - sign));
    };
    return IVec3(sign_extend(id & X_MASK, X_BITS),
                 sign_extend((id >> (X_BITS + Z_BITS)) & Y_MASK, Y_BITS),
                 sign_extend((id >> X_BITS) & Z_MASK, Z_BITS));
}

} // namespace Luminumbra
