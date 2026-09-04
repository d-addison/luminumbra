#pragma once

#include <cstdint>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Luminumbra {

// --- Core Identifiers ---
using EntityID = entt::entity;
using ChunkID = uint64_t;
using MeshID = uint32_t;
using MaterialID = uint32_t;
using TextureID = uint32_t;
using SoundID = uint32_t;

// --- Numeric Types ---
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;

// --- GLM Aliases ---
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using IVec2 = glm::ivec2;
using IVec3 = glm::ivec3;
using IVec4 = glm::ivec4;
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Quat = glm::quat;

// --- Engine Constants ---
constexpr u32 TICKS_PER_SECOND = 30;
constexpr f32 SECONDS_PER_TICK = 1.0f / static_cast<f32>(TICKS_PER_SECOND);

// World dimensions
constexpr i32 CHUNK_SIZE_X = 16;
constexpr i32 CHUNK_SIZE_Y = 16;
constexpr i32 CHUNK_SIZE_Z = 16;
constexpr i32 CHUNK_VOLUME = CHUNK_SIZE_X * CHUNK_SIZE_Y * CHUNK_SIZE_Z;

// Water Simulation Grid Resolution (per chunk)
constexpr i32 WATER_SIM_RESOLUTION_X = 8;
constexpr i32 WATER_SIM_RESOLUTION_Z = 8;
constexpr f32 SEA_LEVEL = 0.0f;
const int RENDER_DISTANCE = 32;
const int RENDER_DISTANCE_UP = 8;
const int RENDER_DISTANCE_DOWN = 16;

constexpr f32 NEAR_FIELD_DISTANCE = 256.0f;
constexpr f32 FAR_FIELD_DISTANCE = 8192.0f;

enum class MaterialType : u32 {
    Air = 0,
    Stone = 1,
    Soil = 2,
    Grass = 3,
    Sand = 4,
    Deepslate = 5,
    LuminCrystal = 6,
    Water = 7
};

} // namespace Luminumbra