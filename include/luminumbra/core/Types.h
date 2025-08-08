#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Luminumbra {

// --- Core Identifiers ---

// The unique identifier for any object in the world, managed by EnTT.
using EntityID = entt::entity;

// A unique identifier for a specific chunk in the world grid.
// Can be calculated from a chunk's 3D integer coordinates.
using ChunkID = uint64_t;

// Identifiers for assets loaded into memory.
using MeshID = uint32_t;
using MaterialID = uint32_t;
using TextureID = uint32_t;
using SoundID = uint32_t;


// --- Numeric Types ---

// Use fixed-width integers for all simulation and serialization code
// to guarantee determinism and predictable memory layouts.
using i8  = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// Standard floating point types for rendering, where precision is less
// critical than performance. Not to be used in core simulation logic.
using f32 = float;
using f64 = double;


// --- GLM Aliases for Readability & Intent ---

// Vectors
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;

// Integer Vectors (useful for coordinates, e.g., voxel or chunk positions)
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;

// Matrices
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;

// Quaternions for rotation
using Quat = glm::quat;


// --- Engine Constants ---

// The fixed simulation tick rate, as defined in the README.
constexpr u32 TICKS_PER_SECOND = 30;
constexpr f32 SECONDS_PER_TICK = 1.0f / static_cast<f32>(TICKS_PER_SECOND);

// World dimensions
constexpr i32 CHUNK_SIZE_X = 32;
constexpr i32 CHUNK_SIZE_Y = 32;
constexpr i32 CHUNK_SIZE_Z = 32;
constexpr i32 CHUNK_VOLUME = CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z;

// Rendering distances
constexpr f32 NEAR_FIELD_DISTANCE = 256.0f;
constexpr f32 FAR_FIELD_DISTANCE = 8192.0f; // Max render distance for SDFs

} // namespace Luminumbra
