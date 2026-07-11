#include "Chunk.h"
#include "../core/Log.h"

namespace Luminumbra {

Chunk::Chunk(const IVec3& coords)
    : m_coords(coords),
      m_id(calculate_id(coords)),
      m_state(ChunkState::Unloaded) {
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
    return IVec3(
        sign_extend(id & X_MASK, X_BITS),
        sign_extend((id >> (X_BITS + Z_BITS)) & Y_MASK, Y_BITS),
        sign_extend((id >> X_BITS) & Z_MASK, Z_BITS));
}

} // namespace Luminumbra
