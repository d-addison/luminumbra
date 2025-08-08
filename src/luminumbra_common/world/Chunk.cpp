#include "Chunk.h"

namespace Luminumbra {

Chunk::Chunk(const IVec3& coords)
    : m_coords(coords),
      m_id(calculate_id(coords)),
      m_state(ChunkState::Unloaded) {
}

// In src/luminumbra_common/world/Chunk.cpp

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

} // namespace Luminumbra
