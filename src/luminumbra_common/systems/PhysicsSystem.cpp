#include "PhysicsSystem.h"
#include "../../../include/luminumbra/core/Types.h"
#include "../world/Chunk.h"
#include "../world/GameSession.h"
#include "../world/PlayerAvatar.h"
#include "SHIELD_WorldSystem.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Geometry/Sphere.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/StateRecorder.h>
#include <Jolt/RegisterTypes.h>

#include "../core/Log.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace JPH;

namespace Luminumbra {
namespace Systems {

namespace {
// These internal body snapshots bridge Jolt's typed state calls. They never
// enter wire or hash inputs; optional persistence wraps them in its own envelope.
// Scalar encoding excludes SIMD
// padding, enum widths and host endianness from diagnostic comparisons.
// Positions and sleep offsets are float64 words. Single-precision Jolt writes
// only float32-exact positions and zero offsets, but double-precision Jolt can
// write values a single-precision build cannot restore exactly. Jolt precision
// is therefore part of the version: each build refuses the other's states at
// the header instead of partway through decoding.
#ifdef JPH_DOUBLE_PRECISION
constexpr std::uint32_t kOwnerVersion = 0x00010002u;
#else
constexpr std::uint32_t kOwnerVersion = 2;
#endif
constexpr std::uint32_t kFrozenBodyOwner = 1;
constexpr std::uint32_t kSphereOwner = 3;
constexpr std::uint32_t kMaxOwnerContacts = 256;
constexpr std::uint32_t kMaxOwnerPairs = 64;
constexpr std::uint32_t kPhysicsCapacity = 10240;
struct OwnerStateRefusal {};

class OwnerNativeStream final : public JPH::StateRecorder {
public:
    void WriteBytes(const void* data, std::size_t count) override {
        if (count > bytes.size() - size) {
            throw OwnerStateRefusal{};
        }
        std::memcpy(bytes.data() + size, data, count);
        size += count;
    }
    void ReadBytes(void* data, std::size_t count) override {
        if (count > size - cursor) {
            throw OwnerStateRefusal{};
        }
        std::memcpy(data, bytes.data() + cursor, count);
        cursor += count;
    }
    bool IsEOF() const override {
        return cursor == size;
    }
    bool IsFailed() const override {
        return false;
    }
    // Both native scratch storage and the canonical payload have a 64 KiB cap.
    std::array<std::uint8_t, PhysicsSystem::BodyOwnerState::kMaxBytes> bytes{};
    std::size_t size = 0;
    std::size_t cursor = 0;
};

class OwnerCodec {
public:
    template<class State>
    explicit OwnerCodec(State& output)
        : m_output(output.bytes)
        , m_output_size(&output.size) {}
    template<class State>
    explicit OwnerCodec(const State& input)
        : m_input(input.bytes)
        , m_input_size(input.size) {
        require(input.size <= input.bytes.size());
    }
    static void require(bool condition) {
        if (!condition) {
            throw OwnerStateRefusal{};
        }
    }
    bool reading() const {
        return m_output_size == nullptr;
    }
    void finish() {
        if (reading()) {
            require(m_cursor == m_input_size);
        } else {
            *m_output_size = static_cast<std::uint32_t>(m_cursor);
        }
    }
    std::uint32_t word(std::uint32_t value = 0) {
        const auto limit = reading() ? m_input_size : m_output.size();
        require(m_cursor <= limit && 4 <= limit - m_cursor);
        if (reading()) {
            value = 0;
            for (unsigned i = 0; i < 4; ++i) {
                value |= static_cast<std::uint32_t>(m_input[m_cursor++]) << (8 * i);
            }
        } else {
            for (unsigned i = 0; i < 4; ++i) {
                m_output[m_cursor++] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        }
        return value;
    }
    void require_words(std::uint32_t count) const {
        const auto limit = reading() ? m_input_size : m_output.size();
        require(m_cursor <= limit && count <= (limit - m_cursor) / 4);
    }
    float number(float value = 0.0f) {
        value = std::bit_cast<float>(word(std::bit_cast<std::uint32_t>(value)));
        require(std::isfinite(value));
        return value;
    }
    double wide_number(double value = 0.0) {
        const auto bits = std::bit_cast<std::uint64_t>(value);
        const auto low = word(static_cast<std::uint32_t>(bits));
        const auto high = word(static_cast<std::uint32_t>(bits >> 32));
        value = std::bit_cast<double>((static_cast<std::uint64_t>(high) << 32) | low);
        require(std::isfinite(value));
        return value;
    }
    template<class T>
    T integer(OwnerNativeStream& native, std::uint32_t maximum) {
        T value{};
        if (!reading()) {
            native.Read(value);
        }
        const auto result = word(static_cast<std::uint32_t>(value));
        require(result <= maximum);
        value = static_cast<T>(result);
        if (reading()) {
            native.Write(value);
        }
        return value;
    }
    float scalar(OwnerNativeStream& native) {
        float value = 0.0f;
        if (!reading()) {
            native.Read(value);
        }
        value = number(value);
        if (reading()) {
            native.Write(value);
        }
        return value;
    }
    JPH::Vec3 vector(OwnerNativeStream& native) {
        JPH::Vec3 value = JPH::Vec3::sZero();
        if (!reading()) {
            native.Read(value);
        }
        const float x = number(value.GetX());
        const float y = number(value.GetY());
        const float z = number(value.GetZ());
        value = JPH::Vec3(x, y, z);
        if (reading()) {
            native.Write(value);
        }
        return value;
    }
    JPH::RVec3 position(OwnerNativeStream& native) {
        JPH::RVec3 value = JPH::RVec3::sZero();
        if (!reading()) {
            native.Read(value);
        }
        std::array<JPH::Real, 3> components{value.GetX(), value.GetY(), value.GetZ()};
        for (auto& component : components) {
#ifdef JPH_DOUBLE_PRECISION
            auto bits = std::bit_cast<std::uint64_t>(component);
#else
            auto bits = widen_float_bits(std::bit_cast<std::uint32_t>(component));
#endif
            const auto low = word(static_cast<std::uint32_t>(bits));
            const auto high = word(static_cast<std::uint32_t>(bits >> 32));
            bits = (static_cast<std::uint64_t>(high) << 32) | low;
            require(((bits >> 52) & 2047u) != 2047u);
#ifdef JPH_DOUBLE_PRECISION
            component = std::bit_cast<double>(bits);
#else
            component = std::bit_cast<float>(narrow_float_bits(bits));
#endif
            // Public owner coordinates stay within the float32 range even with
            // float64 Jolt positions (whose exact values a precision-specific
            // kOwnerVersion keeps apart). Refuse out-of-range input before any
            // later narrowing.
            constexpr auto max_coordinate =
                static_cast<JPH::Real>((std::numeric_limits<float>::max)());
            require(component >= -max_coordinate && component <= max_coordinate);
        }
        if (reading()) {
            value = JPH::RVec3(components[0], components[1], components[2]);
            native.Write(value);
        }
        return value;
    }
    void normal(OwnerNativeStream& native, bool allow_zero = false) {
        const auto value = vector(native);
        require(value.IsNormalized() || (allow_zero && value == JPH::Vec3::sZero()));
    }
    void quaternion(OwnerNativeStream& native) {
        JPH::Quat value = JPH::Quat::sIdentity();
        if (!reading()) {
            native.Read(value);
        }
        const float x = number(value.GetX());
        const float y = number(value.GetY());
        const float z = number(value.GetZ());
        const float w = number(value.GetW());
        const JPH::Quat rotation(x, y, z, w);
        require(rotation.IsNormalized());
        if (reading()) {
            native.Write(rotation);
        }
    }
    std::uint32_t body_id(OwnerNativeStream& native) {
        JPH::BodyID value;
        if (!reading()) {
            native.Read(value);
        }
        const auto id = checked_body_id(word(value.GetIndexAndSequenceNumber()));
        if (reading()) {
            native.Write(JPH::BodyID(id));
        }
        return id;
    }
    static std::uint32_t checked_body_id(std::uint32_t id) {
        require(id == JPH::BodyID::cInvalidBodyID ||
                ((id & JPH::BodyID::cBroadPhaseBit) == 0 &&
                 (id & JPH::BodyID::cMaxBodyIndex) < kPhysicsCapacity));
        return id;
    }
    void floats(OwnerNativeStream& native, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            scalar(native);
        }
    }

private:
#ifndef JPH_DOUBLE_PRECISION
    // Integer IEEE-754 conversion keeps signed zero and subnormals exact even
    // when the simulation thread enables flush-to-zero/denormals-are-zero.
    static std::uint64_t widen_float_bits(std::uint32_t bits) {
        const auto sign = static_cast<std::uint64_t>(bits & 0x80000000u) << 32;
        const auto exponent = (bits >> 23) & 255u;
        const auto fraction = bits & 0x7fffffu;
        require(exponent != 255u);
        if (exponent != 0) {
            return sign | (static_cast<std::uint64_t>(exponent + 896u) << 52) |
                   (static_cast<std::uint64_t>(fraction) << 29);
        }
        if (fraction == 0) {
            return sign;
        }
        const auto leading = static_cast<unsigned>(std::bit_width(fraction)) - 1u;
        if (leading > 22u) {
            throw OwnerStateRefusal{};
        }
        return sign | (static_cast<std::uint64_t>(leading + 874u) << 52) |
               (static_cast<std::uint64_t>(fraction - (1u << leading)) << (52u - leading));
    }
    static std::uint32_t narrow_float_bits(std::uint64_t bits) {
        const auto sign = static_cast<std::uint32_t>(bits >> 32) & 0x80000000u;
        const auto exponent = static_cast<std::uint32_t>((bits >> 52) & 2047u);
        const auto fraction = bits & ((std::uint64_t{1} << 52) - 1);
        if (exponent == 0) {
            require(fraction == 0);
            return sign;
        }
        if (exponent >= 897u && exponent <= 1150u) {
            require((fraction & ((std::uint64_t{1} << 29) - 1)) == 0);
            return sign | ((exponent - 896u) << 23) | static_cast<std::uint32_t>(fraction >> 29);
        }
        if (exponent < 874u || exponent > 896u) {
            throw OwnerStateRefusal{};
        }
        const auto leading = exponent - 874u;
        const auto shift = 52u - leading;
        require((fraction & ((std::uint64_t{1} << shift) - 1)) == 0);
        return sign | (1u << leading) | static_cast<std::uint32_t>(fraction >> shift);
    }
#endif
    std::span<const std::uint8_t> m_input;
    std::span<std::uint8_t> m_output;
    std::uint32_t* m_output_size = nullptr;
    std::size_t m_input_size = 0;
    std::size_t m_cursor = 0;
};

struct OwnerRecipe {
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    glm::vec2 wish{0.0f};
    std::uint32_t slot_count = 0;
    std::vector<std::uint32_t> free_slots;
    std::uint32_t body = JPH::BodyID::cInvalidBodyID;
    float radius = 0.0f;
    JPH::RVec3 feet = JPH::RVec3::sZero();
    OwnerNativeStream owner;
    OwnerNativeStream scene;
    std::array<std::uint32_t, 4> global_bits{};
    std::size_t scene_contacts_offset = 0;
    // Ground + 256 character contacts, or 2*64 pairs + 2*256 CCD references.
    std::array<std::uint32_t, 640> references{};
    std::size_t reference_count = 0;
    using ContactKey = std::array<std::uint32_t, 4>;
    std::array<ContactKey, kMaxOwnerPairs> pairs{};
    std::array<ContactKey, 2 * kMaxOwnerContacts> contacts{};
    std::size_t pair_count = 0;
    std::size_t contact_count = 0;
    void reference(std::uint32_t id) {
        if (id != JPH::BodyID::cInvalidBodyID) {
            OwnerCodec::require(reference_count < references.size());
            references[reference_count++] = id;
        }
    }
};

void CharacterSchema(OwnerCodec& codec, OwnerRecipe& recipe) {
    auto& native = recipe.owner;
    codec.integer<JPH::CharacterBase::EGroundState>(native, 3);
    recipe.reference(codec.body_id(native));
    codec.integer<std::uint32_t>(native, UINT32_MAX); // ground subshape
    static_cast<void>(codec.position(native));
    codec.normal(native, true);              // no ground contact initially has a zero normal
    static_cast<void>(codec.vector(native)); // ground velocity
    recipe.feet = codec.position(native);
    codec.quaternion(native);
    static_cast<void>(codec.vector(native));           // linear velocity
    OwnerCodec::require(codec.scalar(native) >= 0.0f); // previous delta time
    codec.integer<bool>(native, 1);                    // hit limit reached
    const auto count = codec.integer<std::uint32_t>(native, kMaxOwnerContacts);
    for (std::uint32_t i = 0; i < count; ++i) {
        recipe.reference(codec.body_id(native));
        // No character-vs-character provider is installed by either controller path.
        const auto character = codec.integer<std::uint32_t>(native, UINT32_MAX);
        OwnerCodec::require(character == UINT32_MAX);
        codec.integer<std::uint32_t>(native, UINT32_MAX); // subshape
        static_cast<void>(codec.position(native));
        static_cast<void>(codec.vector(native)); // contact velocity
        codec.normal(native);                    // contact normal
        codec.normal(native);                    // surface normal
        codec.floats(native, 2);                 // distance, fraction
        codec.integer<JPH::EMotionType>(native, 2);
        codec.integer<bool>(native, 1); // sensor
        codec.integer<bool>(native, 1); // collided
        codec.integer<bool>(native, 1); // discarded
        codec.integer<bool>(native, 1); // can push
    }
}

void BodySchema(OwnerCodec& codec, OwnerNativeStream& native) {
    codec.integer<bool>(native, 1); // active list membership
    static_cast<void>(codec.position(native));
    codec.quaternion(native);
    for (unsigned i = 0; i < 4; ++i) {
        static_cast<void>(codec.vector(native));
    } // velocity, spin, force, torque
    // The canonical sleep offset is always three float64 scalars. Jolt omits it
    // in single-precision builds, which can accept only a zero offset.
    for (unsigned i = 0; i < 3; ++i) {
        double offset = 0.0;
#ifdef JPH_DOUBLE_PRECISION
        if (!codec.reading()) {
            native.Read(offset);
        }
#endif
        offset = codec.wide_number(offset);
        constexpr auto max_offset = static_cast<double>((std::numeric_limits<float>::max)());
        OwnerCodec::require(offset >= -max_offset && offset <= max_offset);
#ifdef JPH_DOUBLE_PRECISION
        if (codec.reading()) {
            native.Write(offset);
        }
#else
        OwnerCodec::require(offset == 0.0);
#endif
    }
    // Sleep-test sphere centers and radii, followed by their accumulated timer.
    for (unsigned sphere = 0; sphere < 3; ++sphere) {
        codec.floats(native, 3);
        OwnerCodec::require(codec.scalar(native) >= 0.0f);
    }
    OwnerCodec::require(codec.scalar(native) >= 0.0f); // sleep timer
    codec.integer<bool>(native, 1);
}

OwnerRecipe::ContactKey PairKeySchema(OwnerCodec& codec, OwnerRecipe& recipe, bool subshapes) {
    const auto first = codec.body_id(recipe.scene);
    const auto first_shape =
        subshapes ? codec.integer<std::uint32_t>(recipe.scene, UINT32_MAX) : 0u;
    const auto second = codec.body_id(recipe.scene);
    const auto second_shape =
        subshapes ? codec.integer<std::uint32_t>(recipe.scene, UINT32_MAX) : 0u;
    OwnerCodec::require(first < second && first != JPH::BodyID::cInvalidBodyID &&
                        second != JPH::BodyID::cInvalidBodyID);
    OwnerCodec::require(first == recipe.body || second == recipe.body);
    recipe.reference(first);
    recipe.reference(second);
    return {first, first_shape, second, second_shape};
}

void CheckUniqueKeys(std::span<OwnerRecipe::ContactKey> keys) {
    std::sort(keys.begin(), keys.end());
    OwnerCodec::require(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

void SceneSchema(OwnerCodec& codec, OwnerRecipe& recipe) {
    auto& native = recipe.scene;
    const auto flags = codec.integer<JPH::EStateRecorderState>(native, 15);
    OwnerCodec::require(flags ==
                        (JPH::EStateRecorderState::Global | JPH::EStateRecorderState::Contacts));
    const auto delta = codec.scalar(native);
    OwnerCodec::require(delta >= 0.0f); // warm-start step delta
    const auto gravity = codec.vector(native);
    recipe.global_bits = {std::bit_cast<std::uint32_t>(delta),
                          std::bit_cast<std::uint32_t>(gravity.GetX()),
                          std::bit_cast<std::uint32_t>(gravity.GetY()),
                          std::bit_cast<std::uint32_t>(gravity.GetZ())};
    recipe.scene_contacts_offset = codec.reading() ? native.size : native.cursor;
    const auto pairs = codec.integer<std::uint32_t>(native, kMaxOwnerPairs);
    std::uint32_t total_manifolds = 0;
    for (std::uint32_t i = 0; i < pairs; ++i) {
        const auto parent = PairKeySchema(codec, recipe, false);
        recipe.pairs[recipe.pair_count++] = parent;
        codec.floats(native, 3); // relative position
        const auto rotation = codec.vector(native);
        // Jolt reconstructs w = sqrt(max(1 - |xyz|^2, 0)). Permit float32
        // rounding at half turns, but never publish a non-unit cached rotation.
        // 1e-4 allows float32 product/compression error near half turns while
        // rejecting length errors that would distort Jolt's cache dot product.
        const auto xyz_squared = rotation.LengthSq();
        const auto w = std::sqrt(std::max(1.0f - xyz_squared, 0.0f));
        OwnerCodec::require(std::isfinite(xyz_squared) &&
                            std::abs(1.0f - (xyz_squared + w * w)) <= 1.0e-4f);
        const auto manifolds = codec.integer<std::uint32_t>(native, kMaxOwnerContacts);
        OwnerCodec::require(manifolds <= kMaxOwnerContacts - total_manifolds);
        total_manifolds += manifolds;
        for (std::uint32_t j = 0; j < manifolds; ++j) {
            // SubShapeIDPair consists of four scalar IDs, never padding.
            // Pair references were already recorded; keep the reference bound fixed.
            const auto previous_count = recipe.reference_count;
            const auto key = PairKeySchema(codec, recipe, true);
            OwnerCodec::require(key[0] == parent[0] && key[2] == parent[2]);
            recipe.contacts[recipe.contact_count++] = key;
            recipe.reference_count = previous_count;
            const auto points = codec.integer<JPH::uint16>(native, 4);
            OwnerCodec::require(points != 0);
            const auto normal_x = codec.scalar(native);
            const auto normal_y = codec.scalar(native);
            const auto normal_z = codec.scalar(native);
            OwnerCodec::require(JPH::Vec3(normal_x, normal_y, normal_z).IsNormalized());
            for (JPH::uint16 point = 0; point < points; ++point) {
                codec.floats(native, 9); // two positions, normal impulse, two friction impulses
            }
        }
    }
    const auto ccd = codec.integer<std::uint32_t>(native, kMaxOwnerContacts);
    for (std::uint32_t i = 0; i < ccd; ++i) {
        recipe.contacts[recipe.contact_count++] = PairKeySchema(codec, recipe, true);
    }
    CheckUniqueKeys(std::span(recipe.pairs).first(recipe.pair_count));
    CheckUniqueKeys(std::span(recipe.contacts).first(recipe.contact_count));
}

void OwnerSchema(OwnerCodec& codec, OwnerRecipe& recipe) {
    OwnerCodec::require(codec.word(kOwnerVersion) == kOwnerVersion);
    recipe.kind = codec.word(recipe.kind);
    OwnerCodec::require(recipe.kind == kFrozenBodyOwner || recipe.kind == kSphereOwner);
    if (recipe.kind == kSphereOwner) {
        recipe.body = OwnerCodec::checked_body_id(codec.word(recipe.body));
        OwnerCodec::require(recipe.body != JPH::BodyID::cInvalidBodyID);
        recipe.radius = codec.number(recipe.radius);
        OwnerCodec::require(recipe.radius > 0.0f);
        // Reconstruct only radii whose default mass/inertia can form a dynamic
        // body. Finite radii alone can still underflow mass or overflow inertia.
        const auto properties = JPH::SphereShape(recipe.radius).GetMassProperties();
        const auto inertia = properties.mInertia.GetDiagonal3();
        OwnerCodec::require(std::isfinite(properties.mMass) && properties.mMass > 0.0f &&
                            std::isfinite(2.5f / properties.mMass) &&
                            std::isfinite(inertia.GetX()) && inertia.GetX() > 0.0f &&
                            std::isfinite(1.0f / inertia.GetX()) && std::isfinite(inertia.GetY()) &&
                            inertia.GetY() > 0.0f && std::isfinite(1.0f / inertia.GetY()) &&
                            std::isfinite(inertia.GetZ()) && inertia.GetZ() > 0.0f &&
                            std::isfinite(1.0f / inertia.GetZ()));
        BodySchema(codec, recipe.owner);
        SceneSchema(codec, recipe);
    } else {
        recipe.body = OwnerCodec::checked_body_id(codec.word(recipe.body));
        OwnerCodec::require(recipe.body != JPH::BodyID::cInvalidBodyID);
        recipe.feet = codec.position(recipe.owner);
    }
    codec.finish();
    if (!codec.reading()) {
        OwnerCodec::require(recipe.owner.IsEOF() && recipe.scene.IsEOF());
    }
}

// Controller scalar schema is version 1; body snapshots retain independent
// version 2. Persistence wraps these payloads; wire and hash inputs are unchanged.
void ControllerSchema(OwnerCodec& codec, OwnerRecipe& recipe) {
#ifdef JPH_DOUBLE_PRECISION
    constexpr std::uint32_t version = 0x00010001u;
#else
    constexpr std::uint32_t version = 1;
#endif
    OwnerCodec::require(codec.word(version) == version);
    recipe.kind = codec.word(recipe.kind);
    OwnerCodec::require(recipe.kind == 1u || recipe.kind == 2u);
    recipe.flags = codec.word(recipe.flags);
    OwnerCodec::require(recipe.kind == 1u ? recipe.flags <= 1u
                                          : (recipe.flags == 0u || recipe.flags == 2u));
    recipe.wish.x = codec.number(recipe.wish.x);
    recipe.wish.y = codec.number(recipe.wish.y);
    OwnerCodec::require((recipe.kind == 1u && recipe.flags == 0u) ||
                        recipe.wish == glm::vec2(0.0f));
    recipe.body = OwnerCodec::checked_body_id(codec.word(recipe.body));
    OwnerCodec::require((recipe.kind == 1u && recipe.flags == 1u) ||
                        recipe.body == JPH::BodyID::cInvalidBodyID);
    CharacterSchema(codec, recipe);
    if (recipe.body != JPH::BodyID::cInvalidBodyID) {
        recipe.slot_count = codec.word(recipe.slot_count);
        OwnerCodec::require(recipe.slot_count <= kPhysicsCapacity &&
                            JPH::BodyID(recipe.body).GetIndex() < recipe.slot_count);
        const auto count = codec.word(static_cast<std::uint32_t>(recipe.free_slots.size()));
        OwnerCodec::require(count < recipe.slot_count);
        codec.require_words(count);
        if (codec.reading()) {
            recipe.free_slots.resize(count);
        }
        for (auto& slot : recipe.free_slots) {
            slot = codec.word(slot);
        }
    }
    codec.finish();
}

bool ParseController(const World::PhysicsOwnerState& state, OwnerRecipe& recipe) {
    if (!World::ValidatePhysicsOwner(state)) {
        return false;
    }
    try {
        OwnerCodec codec(state);
        ControllerSchema(codec, recipe);
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool WriteController(OwnerRecipe& recipe, World::PhysicsOwnerState& output) {
    try {
        World::PhysicsOwnerState staged;
        OwnerCodec codec(staged);
        ControllerSchema(codec, recipe);
        if (!World::ValidatePhysicsOwner(staged)) {
            return false;
        }
        output = staged;
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool ParseOwner(const PhysicsSystem::BodyOwnerState& state, OwnerRecipe& recipe) {
    try {
        OwnerCodec codec(state);
        OwnerSchema(codec, recipe);
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool ValidateOwnerContactCache(std::vector<OwnerRecipe>& recipes,
                               const JPH::PhysicsSettings& settings) {
    JPH::ContactConstraintManager cache(settings);
    cache.Init(kPhysicsCapacity, kPhysicsCapacity);
    // At most 128 parts, 64 pairs and 256 manifolds plus 256 CCD entries per
    // part. Use Jolt's real allocator: counts omit entry headers, alignment and
    // the unused tail of each part's allocator block.
    try {
        for (auto& recipe : recipes) {
            if (recipe.kind == kSphereOwner) {
                recipe.scene.cursor = recipe.scene_contacts_offset;
                recipe.scene.SetIsLastPart(false);
                if (!cache.RestoreState(recipe.scene, nullptr) || !recipe.scene.IsEOF()) {
                    return false;
                }
                recipe.scene.cursor = 0;
            }
        }
    } catch (const OwnerStateRefusal&) {
        // Jolt can stop reading a pair after its allocation fails.
        return false;
    }
    return true;
}

struct UnassignedOwnerBodyDeleter {
    JPH::BodyInterface* body_interface;
    void operator()(JPH::Body* body) const {
        body_interface->DestroyBodyWithoutID(body);
    }
};
using UnassignedOwnerBody = std::unique_ptr<JPH::Body, UnassignedOwnerBodyDeleter>;

// Requires the body to own `id` already. Afterwards the slot's sequence counter
// matches the restored generation, so the slot reuses exactly the generations
// an uninterrupted run would allocate next.
void CycleRestoredBodySequence(JPH::BodyInterface& body_interface,
                               JPH::Body* body,
                               JPH::BodyID id) {
    // Explicit IDs do not update Jolt's private sequence counter. Unassigning
    // puts this slot first on the free list; ordinary assignment advances that
    // same slot without allocating or changing the other free slots' order.
    // At most 256 increments reach any uint8 generation, including wrap to 0.
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        body = body_interface.UnassignBodyID(body->GetID());
        if (!body_interface.AssignBodyID(body)) {
            throw std::logic_error("Validated owner body slot could not be reassigned");
        }
        if (body->GetID() == id) {
            return;
        }
    }
    throw std::logic_error("Owner body sequence did not cycle");
}

void AssignRestoredOwnerBody(JPH::BodyInterface& body_interface, JPH::Body* body, JPH::BodyID id) {
    // Admission checked every occupied slot, irrespective of its generation.
    // Owner-thread exclusion makes assignment infallible after preparation.
    if (!body_interface.AssignBodyID(body, id)) {
        throw std::logic_error("Validated owner body slot became occupied");
    }
    CycleRestoredBodySequence(body_interface, body, id);
}

// Restored terrain colliders use the same sequence restore as owner bodies. A refused
// slot destroys the unassigned body and returns null, like CreateBodyWithID.
JPH::Body* CreateRestoredBody(JPH::BodyInterface& body_interface,
                              const JPH::BodyCreationSettings& settings,
                              JPH::BodyID id) {
    JPH::Body* body = body_interface.CreateBodyWithoutID(settings);
    if (body == nullptr) {
        return nullptr;
    }
    if (!body_interface.AssignBodyID(body, id)) {
        body_interface.DestroyBodyWithoutID(body);
        return nullptr;
    }
    CycleRestoredBodySequence(body_interface, body, id);
    return body;
}

// Validate the complete pool topology before allocating or publishing any owner.
// At most 10240 slots, 128 body recipes, 4095 controller reservations and
// 10240 existing bodies are examined.
bool ValidateOwnerAllocator(const PhysicsSystem::BodySetState& state,
                            std::span<const OwnerRecipe> recipes,
                            const JPH::BodyIDVector& existing,
                            std::uint32_t destination_slots,
                            std::span<const std::uint32_t> reserved) {
    if (state.slot_count > kPhysicsCapacity || destination_slots > state.slot_count ||
        state.free_slots.size() > state.slot_count) {
        return false; // Jolt cannot shrink its already allocated slot array.
    }
    std::array<bool, kPhysicsCapacity> seen{};
    const auto mark = [&seen, &state](std::uint32_t id) {
        const auto index = id & JPH::BodyID::cMaxBodyIndex;
        if ((id & JPH::BodyID::cBroadPhaseBit) != 0 || index >= state.slot_count || seen[index]) {
            return false;
        }
        seen[index] = true;
        return true;
    };
    for (const auto slot : state.free_slots) {
        if (!mark(slot)) {
            return false;
        }
    }
    for (const auto id : existing) {
        if (!mark(id.GetIndexAndSequenceNumber())) {
            return false;
        }
    }
    for (const auto& recipe : recipes) {
        if (!mark(recipe.body)) {
            return false;
        }
    }
    for (const auto id : reserved) {
        if (!mark(id)) {
            return false;
        }
    }
    return std::all_of(
        seen.begin(), seen.begin() + state.slot_count, [](bool used) { return used; });
}

UnassignedOwnerBody PrepareOwnerAllocatorScratch(JPH::BodyInterface& body_interface,
                                                 bool needed,
                                                 JPH::ObjectLayer layer) {
    UnassignedOwnerBody scratch(nullptr, UnassignedOwnerBodyDeleter{&body_interface});
    if (needed) {
        const JPH::BodyCreationSettings settings(new JPH::SphereShape(0.3f),
                                                 JPH::RVec3::sZero(),
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 layer);
        scratch.reset(body_interface.CreateBodyWithoutID(settings));
    }
    return scratch;
}

void RestoreOwnerFreeSlots(JPH::BodyInterface& body_interface,
                           JPH::Body* scratch,
                           std::span<const std::uint32_t> free_slots) {
    // Reusing one unadded body avoids allocations during commit. Cycling each
    // bounded uint8 sequence also handles generations left in the destination.
    for (const auto slot : free_slots) {
        AssignRestoredOwnerBody(body_interface, scratch, JPH::BodyID(slot));
        scratch = body_interface.UnassignBodyID(scratch->GetID());
    }
}

bool WriteOwner(OwnerRecipe& recipe, PhysicsSystem::BodyOwnerState& output) {
    try {
        PhysicsSystem::BodyOwnerState staged;
        OwnerCodec codec(staged);
        OwnerSchema(codec, recipe);
        output = staged;
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

class OwnerContactFilter final : public JPH::StateRecorderFilter {
public:
    OwnerContactFilter(JPH::BodyID owner, const JPH::PhysicsSystem& system)
        : m_owner(owner)
        , m_system(system) {}
    bool ShouldSaveContact(const JPH::BodyID& first, const JPH::BodyID& second) const override {
        if (first != m_owner && second != m_owner) {
            return false;
        }
        const auto other = first == m_owner ? second : first;
        const JPH::BodyLockRead lock(m_system.GetBodyLockInterface(), other);
        // Each pair belongs to its lower-ID dynamic body, or its sole dynamic
        // body when the other side is terrain/a frozen capsule. No duplicates
        // when a complete owner set is restored in multiple Jolt parts.
        return lock.Succeeded() && (lock.GetBody().IsStatic() || m_owner < other);
    }

private:
    JPH::BodyID m_owner;
    const JPH::PhysicsSystem& m_system;
};

bool ReferencesExist(const OwnerRecipe& recipe,
                     const JPH::PhysicsSystem& system,
                     std::span<const std::uint32_t> reserved) {
    // At most 257 controller references and 4095 validated capsule reservations.
    for (std::size_t i = 0; i < recipe.reference_count; ++i) {
        if (std::find(reserved.begin(), reserved.end(), recipe.references[i]) != reserved.end()) {
            continue;
        }
        const JPH::BodyLockRead lock(system.GetBodyLockInterface(),
                                     JPH::BodyID(recipe.references[i]));
        if (!lock.Succeeded()) {
            return false;
        }
    }
    return true;
}

JPH::Ref<JPH::Shape> FeetCapsule(float half_height, float offset) {
    const JPH::Ref<JPH::Shape> capsule =
        JPH::CapsuleShapeSettings(half_height, 0.4f).Create().Get();
    return JPH::RotatedTranslatedShapeSettings(
               JPH::Vec3(0.0f, offset, 0.0f), JPH::Quat::sIdentity(), capsule.GetPtr())
        .Create()
        .Get();
}

std::unique_ptr<JPH::CharacterVirtual> PrepareController(const JPH::Ref<JPH::Shape>& shape,
                                                         const glm::vec3& feet,
                                                         JPH::PhysicsSystem* system) {
    JPH::CharacterVirtualSettings settings;
    settings.mShape = shape;
    settings.mMass = 80.0f;
    settings.mMaxSlopeAngle = glm::radians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.4f);
    return std::make_unique<JPH::CharacterVirtual>(
        &settings, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(), system);
}

} // namespace

// --- Jolt Layer Implementations (Unchanged) ---
namespace Layers {
static constexpr ObjectLayer NON_MOVING = 0;
static constexpr ObjectLayer MOVING = 1;
static constexpr ObjectLayer NUM_LAYERS = 2;
} // namespace Layers
namespace BroadPhaseLayers {
static constexpr BroadPhaseLayer NON_MOVING(0);
static constexpr BroadPhaseLayer MOVING(1);
static constexpr uint32 NUM_LAYERS = 2;
} // namespace BroadPhaseLayers
class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        m_object_to_broad[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        m_object_to_broad[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::NUM_LAYERS;
    }
    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override {
        JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
        return m_object_to_broad[inLayer];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override {
        switch ((BroadPhaseLayer::Type)inLayer) {
            case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING:
                return "NON_MOVING";
            case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:
                return "MOVING";
            default:
                JPH_ASSERT(false);
                return "INVALID";
        }
    }
#endif
private:
    BroadPhaseLayer m_object_to_broad[Layers::NUM_LAYERS];
};
class ObjectVsBroadPhaseLayerFilterImpl final : public ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override {
        switch (inLayer1) {
            case Layers::NON_MOVING:
                return inLayer2 == BroadPhaseLayers::MOVING;
            case Layers::MOVING:
                return true;
            default:
                return false;
        }
    }
};
class ObjectLayerPairFilterImpl final : public ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override {
        if (inObject1 == Layers::NON_MOVING && inObject2 == Layers::NON_MOVING)
            return false;
        return true;
    }
};
class BroadPhaseLayerFilterAll final : public BroadPhaseLayerFilter {
public:
    bool ShouldCollide(BroadPhaseLayer) const override {
        return true;
    }
};
class ObjectLayerFilterAll final : public ObjectLayerFilter {
public:
    bool ShouldCollide(ObjectLayer) const override {
        return true;
    }
};

namespace {
struct JoltRuntimeState {
    std::mutex mutex;
    std::size_t users = 0;
    Factory* factory = nullptr;
    bool owns_factory = false;
    RefConst<PhysicsMaterial> prior_material;
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    bool allocators_used = false;
    AllocateFunction used_allocate = nullptr;
    ReallocateFunction used_reallocate = nullptr;
    FreeFunction used_free = nullptr;
    AlignedAllocateFunction used_aligned_allocate = nullptr;
    AlignedFreeFunction used_aligned_free = nullptr;
#endif
};

struct JoltRuntimeIdentity {
    Factory* factory;
    bool owned;
};

JoltRuntimeState& GetJoltRuntimeState() {
    static JoltRuntimeState state;
    return state;
}

void CheckJoltAllocators(JoltRuntimeState& state) {
#ifndef JPH_DISABLE_CUSTOM_ALLOCATOR
    const bool any = Allocate || Reallocate || Free || AlignedAllocate || AlignedFree;
    const bool complete = Allocate && Reallocate && Free && AlignedAllocate && AlignedFree;
    if ((any && !complete) ||
        (!any && (state.allocators_used || state.users != 0 || Factory::sInstance != nullptr))) {
        throw std::logic_error("Incomplete Jolt allocator callbacks");
    }
    if (state.allocators_used && any &&
        (Allocate != state.used_allocate || Reallocate != state.used_reallocate ||
         Free != state.used_free || AlignedAllocate != state.used_aligned_allocate ||
         AlignedFree != state.used_aligned_free)) {
        throw std::logic_error("Jolt allocator callbacks changed after use");
    }
    if (!any) {
        RegisterDefaultAllocator();
    }
    // RTTI retains process-static allocations even after registration fails.
    // Never reset these callbacks to null or replace the set already in use.
    state.allocators_used = true;
    state.used_allocate = Allocate;
    state.used_reallocate = Reallocate;
    state.used_free = Free;
    state.used_aligned_allocate = AlignedAllocate;
    state.used_aligned_free = AlignedFree;
#else
    static_cast<void>(state);
#endif
}

JoltRuntimeIdentity AcquireJoltRuntime() {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if ((state.users != 0 && (state.factory == nullptr || state.factory != Factory::sInstance)) ||
        (state.users == 0 && (state.factory != nullptr || state.owns_factory))) {
        throw std::logic_error("Jolt factory ownership changed during a lease");
    }
    if (state.users == (std::numeric_limits<std::size_t>::max)()) {
        throw std::overflow_error("Jolt runtime lease count overflow");
    }
    // Refuse inconsistent external ownership before changing allocator state.
    if (state.users == 0 && Factory::sInstance != nullptr &&
        (PhysicsMaterial::sDefault == nullptr ||
         Factory::sInstance->Find("PhysicsMaterialSimple") == nullptr)) {
        throw std::logic_error("External Jolt factory is not initialized");
    }
    CheckJoltAllocators(state);
    if (state.users == 0) {
        if (Factory::sInstance == nullptr) {
            const auto prior_material = PhysicsMaterial::sDefault;
            auto factory = std::make_unique<Factory>();
            // RegisterTypes requires a published factory. This temporary
            // publication belongs exclusively to this cold acquisition.
            Factory::sInstance = factory.get();
            try {
                RegisterTypes();
            } catch (...) {
                if (Factory::sInstance != factory.get()) {
                    std::terminate();
                }
                PhysicsMaterial::sDefault = prior_material;
                if (Factory::sInstance != factory.get()) {
                    std::terminate();
                }
                Factory::sInstance = nullptr;
                // The local owner destroys both partially populated maps.
                throw;
            }
            state.prior_material = prior_material;
            state.factory = factory.release();
            state.owns_factory = true;
        } else {
            // The external owner must keep its initialized factory and
            // compatible allocators alive until every borrowed lease ends.
            state.factory = Factory::sInstance;
            state.owns_factory = false;
        }
    }
    ++state.users;
    return {state.factory, state.owns_factory};
}

void ReleaseJoltRuntime(JoltRuntimeIdentity identity) noexcept {
    auto& state = GetJoltRuntimeState();
    std::lock_guard lock(state.mutex);
    if (state.users == 0 || state.factory != identity.factory ||
        state.owns_factory != identity.owned || Factory::sInstance != identity.factory) {
        // A lifetime-contract violation cannot safely unregister any factory.
        std::terminate();
    }
    if (--state.users != 0) {
        return;
    }
    if (state.owns_factory) {
        UnregisterTypes();
        PhysicsMaterial::sDefault = state.prior_material;
        state.prior_material = nullptr;
        if (Factory::sInstance != identity.factory) {
            std::terminate();
        }
        Factory::sInstance = nullptr;
        delete state.factory;
    }
    state.factory = nullptr;
    state.owns_factory = false;
}
} // namespace

class JoltRuntimeLease {
public:
    JoltRuntimeLease() {
        const auto identity = AcquireJoltRuntime();
        m_factory = identity.factory;
        m_owned = identity.owned;
    }
    ~JoltRuntimeLease() {
        ReleaseJoltRuntime({m_factory, m_owned});
    }
    JoltRuntimeLease(const JoltRuntimeLease&) = delete;
    JoltRuntimeLease& operator=(const JoltRuntimeLease&) = delete;

private:
    Factory* m_factory = nullptr;
    bool m_owned = false;
};

namespace {
struct PendingPhysicsResources {
    // Reverse destruction joins/releases jobs before their dependencies and
    // keeps the independent lease alive through every construction failure.
    std::unique_ptr<JoltRuntimeLease> lease;
    std::unique_ptr<TempAllocator> temp;
    std::unique_ptr<JPH::PhysicsSystem> physics;
    std::unique_ptr<JPH::JobSystem> jobs;
};

struct SimulationHorizon {
    // At most 128 distinct columns, plus one candidate owner's nine columns.
    std::array<ChunkID, PhysicsSystem::kSimulationColliderBudget> columns{};
    std::size_t count = 0;

    static bool valid(const Vec3& anchor) {
        if (!std::isfinite(anchor.x) || !std::isfinite(anchor.y) || !std::isfinite(anchor.z)) {
            return false;
        }
        const double cx = std::floor(static_cast<double>(anchor.x) / CHUNK_SIZE_X);
        const double cz = std::floor(static_cast<double>(anchor.z) / CHUNK_SIZE_Z);
        return cx > Chunk::kPackedMinXz && cx < Chunk::kPackedMaxXz && cz > Chunk::kPackedMinXz &&
               cz < Chunk::kPackedMaxXz;
    }

    bool add(const Vec3& anchor) {
        if (!valid(anchor)) {
            return false;
        }
        const auto cx = static_cast<i32>(std::floor(static_cast<double>(anchor.x) / CHUNK_SIZE_X));
        const auto cz = static_cast<i32>(std::floor(static_cast<double>(anchor.z) / CHUNK_SIZE_Z));
        // Stage at most one owner's nine new columns. Duplicate owners copy no
        // existing horizon, and a refused ring leaves the published set untouched.
        std::array<ChunkID, 9> candidate{};
        std::size_t candidate_count = 0;
        const auto end = columns.begin() + static_cast<std::ptrdiff_t>(count);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto id = Chunk::calculate_id(IVec3(cx + dx, 0, cz + dz));
                if (!std::binary_search(columns.begin(), end, id)) {
                    candidate[candidate_count++] = id;
                }
            }
        }
        if (candidate_count > columns.size() - count) {
            return false;
        }
        // The capacity check dominates every insertion; shifting is bounded by
        // 128 columns and runs only for new columns, never per duplicate body.
        for (std::size_t i = 0; i < candidate_count; ++i) {
            const auto merge_end = columns.begin() + static_cast<std::ptrdiff_t>(count);
            const auto found = std::lower_bound(columns.begin(), merge_end, candidate[i]);
            std::move_backward(found, merge_end, merge_end + 1);
            *found = candidate[i];
            ++count;
        }
        return true;
    }

    bool covers(const Vec3& anchor) const {
        if (!valid(anchor)) {
            return false;
        }
        const auto cx = static_cast<i32>(std::floor(static_cast<double>(anchor.x) / CHUNK_SIZE_X));
        const auto cz = static_cast<i32>(std::floor(static_cast<double>(anchor.z) / CHUNK_SIZE_Z));
        const auto end = columns.begin() + static_cast<std::ptrdiff_t>(count);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (!std::binary_search(
                        columns.begin(), end, Chunk::calculate_id(IVec3(cx + dx, 0, cz + dz)))) {
                    return false;
                }
            }
        }
        return true;
    }
};
} // namespace

namespace {
// BodyInterface's velocity setter wakes inactive bodies. Continuation must
// retain both motion and activity, including an inactive body's retained spin.
void SetOwnerVelocityWithoutActivation(JPH::PhysicsSystem& system,
                                       JPH::BodyID id,
                                       JPH::Vec3Arg velocity,
                                       JPH::Vec3Arg angular_velocity) {
    const JPH::BodyLockWrite lock(system.GetBodyLockInterface(), id);
    if (lock.Succeeded()) {
        auto& body = lock.GetBody();
        body.SetLinearVelocityClamped(velocity);
        body.SetAngularVelocityClamped(angular_velocity);
    }
}
} // namespace

// --- PhysicsSystem implementation -------------------------------------------
// Only simulation owners enter this fixed-capacity cache. Render admission never
// consumes its body ids, so asynchronous render order cannot reorder solver inputs.
struct PhysicsSystem::SimulationCollisions {
    static constexpr std::size_t kSamples = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1);
    struct Column {
        ChunkID id = 0;
        std::array<float, kSamples> heights{};
        JPH::RefConst<JPH::Shape> shape;
    };
    JPH::RefConst<JPH::Shape> placeholder;
    std::array<Column, kSimulationColliderBudget> published{};
    std::array<Column, kSimulationColliderBudget> staged{};
    std::array<JPH::BodyID, kSimulationColliderBudget> bodies{};
    // The existing Jolt pool bounds rigid-body registration, independently of
    // character admission. No sphere capacity is borrowed from the 64 characters.
    struct DynamicOwner {
        JPH::BodyID id;
        JPH::Vec3 velocity = JPH::Vec3::sZero();
        JPH::Vec3 angular_velocity = JPH::Vec3::sZero();
        bool suspended = false;
        bool was_active = false;
    };
    std::array<DynamicOwner, 10240> dynamic_owners{};
    // Jolt body index -> registration slot + 1; zero denotes an unregistered body.
    std::array<std::uint32_t, 10240> dynamic_slots{};
    SimulationHorizon horizon;
    std::size_t dynamic_count = 0;
    std::size_t body_count = 0;
    std::size_t published_count = 0;
    std::size_t sampled_count = 0;
    std::size_t owner_visit_count = 0;
    bool ready = false;
    std::uint64_t tick = std::numeric_limits<std::uint64_t>::max();
};

bool PhysicsSystem::simulation_collisions_enabled() const {
    return m_world_system && m_world_system->durable_residency();
}

bool PhysicsSystem::simulation_collisions_ready() const {
    return !simulation_collisions_enabled() || !m_simulation_collisions ||
           m_simulation_collisions->ready;
}

std::uint32_t PhysicsSystem::non_simulation_body_count() const {
    // All private horizon slots, including unpublished reservations, belong to
    // the terrain headroom. They must not reduce capsule admission capacity.
    const auto terrain_slots = m_simulation_collisions ? m_simulation_collisions->body_count : 0;
    return m_jolt_system->GetNumBodies() - static_cast<std::uint32_t>(terrain_slots);
}

void PhysicsSystem::set_world_system(const SHIELD_WorldSystem* world_system) {
    if (world_system != m_world_system) {
        clear_simulation_collisions();
    }
    m_world_system = world_system;
}

void PhysicsSystem::clear_simulation_collisions() {
    if (!m_simulation_collisions) {
        return;
    }
    auto& state = *m_simulation_collisions;
    for (std::size_t i = 0; i < state.dynamic_count; ++i) {
        auto& owner = state.dynamic_owners[i];
        if (owner.suspended) {
            m_body_interface->SetMotionType(
                owner.id, JPH::EMotionType::Dynamic, JPH::EActivation::DontActivate);
            SetOwnerVelocityWithoutActivation(
                *m_jolt_system, owner.id, owner.velocity, owner.angular_velocity);
            if (owner.was_active) {
                m_body_interface->ActivateBody(owner.id);
            }
            owner.suspended = false;
        }
    }
    for (std::size_t i = 0; i < state.body_count; ++i) {
        if (i < state.published_count) {
            m_body_interface->RemoveBody(state.bodies[i]);
        }
        m_body_interface->DestroyBody(state.bodies[i]);
        record_body_release(state.bodies[i]);
    }
    // Dynamic owners outlive a terrain clear. Keep their fixed registration table.
    state.body_count = 0;
    state.published_count = 0;
    state.sampled_count = 0;
    state.owner_visit_count = 0;
    state.horizon = {};
    state.ready = false;
    for (auto& column : state.published) {
        column.shape = nullptr;
    }
    for (auto& column : state.staged) {
        column.shape = nullptr;
    }
}

std::size_t PhysicsSystem::simulation_collision_sample_count() const {
    return m_simulation_collisions ? m_simulation_collisions->sampled_count : 0;
}

std::size_t PhysicsSystem::simulation_collision_owner_visit_count() const {
    return m_simulation_collisions ? m_simulation_collisions->owner_visit_count : 0;
}

std::uint64_t PhysicsSystem::simulation_collision_hash() const {
    // FNV-1a over specified integer words and float bits, in sorted column order.
    std::uint64_t hash = 14695981039346656037ull;
    const auto fold = [&hash](std::uint64_t word) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (word >> (byte * 8)) & 255u;
            hash *= 1099511628211ull;
        }
    };
    if (m_simulation_collisions) {
        const auto& state = *m_simulation_collisions;
        fold(state.published_count);
        for (std::size_t i = 0; i < state.published_count; ++i) {
            fold(state.published[i].id);
            for (const float height : state.published[i].heights) {
                fold(std::bit_cast<std::uint32_t>(height));
            }
        }
    } else {
        fold(0);
    }
    return hash;
}

bool PhysicsSystem::can_admit_simulation_owner(const glm::vec3& position,
                                               bool replacing_player) const {
    if (!simulation_collisions_enabled()) {
        return true;
    }
    const bool keep_player = m_player_character && !replacing_player;
    if (m_live_avatar_count >= kSimulationOwnerBudget - static_cast<std::size_t>(keep_player)) {
        return false;
    }
    SimulationHorizon horizon;
    if (!horizon.add(position) || (keep_player && !horizon.add(get_player_position()))) {
        return false;
    }
    for (auto index = m_avatar_head; index != kNoAvatar; index = m_avatars[index].next) {
        if (!horizon.add(get_avatar_position(index))) {
            return false;
        }
    }
    if (m_simulation_collisions) {
        const auto& state = *m_simulation_collisions;
        for (std::size_t i = 0; i < state.dynamic_count; ++i) {
            if (!horizon.add(get_body_position(state.dynamic_owners[i].id))) {
                return false;
            }
        }
    }
    return true;
}

bool PhysicsSystem::prepare_simulation_collisions(std::span<const Vec3> anchors,
                                                  std::uint64_t tick) try {
    if (!simulation_collisions_enabled()) {
        return true; // Preserve the legacy path, including body allocation order and hashes.
    }
    if (!m_simulation_collisions) {
        m_simulation_collisions = std::make_unique<SimulationCollisions>();
    }
    auto& state = *m_simulation_collisions;
    if (state.ready && tick != std::numeric_limits<std::uint64_t>::max() && state.tick == tick) {
        return true;
    }
    state.ready = false;
    state.sampled_count = 0;
    state.owner_visit_count = 0;
    if (!m_body_interface || anchors.size() > kSimulationOwnerBudget) {
        return false;
    }
    SimulationHorizon horizon;
    for (const auto& anchor : anchors) {
        ++state.owner_visit_count;
        if (!horizon.add(anchor)) {
            return false;
        }
    }
    // Validate every participating position, but do not stall an existing world
    // when movement or authority activation puts its population over budget.
    // Complete rings are admitted in stable owner order; sampling stays <=128
    // columns x 289 heights, with no resident-world traversal or growing scratch.
    const auto include_existing = [&horizon, &state](const Vec3& position) {
        ++state.owner_visit_count;
        if (!SimulationHorizon::valid(position)) {
            return false;
        }
        (void)horizon.add(position);
        return true;
    };
    if (m_player_character && !include_existing(get_player_position())) {
        return false;
    }
    for (auto index = m_avatar_head; index != kNoAvatar; index = m_avatars[index].next) {
        if (!include_existing(get_avatar_position(index))) {
            return false;
        }
    }
    for (std::size_t i = 0; i < state.dynamic_count; ++i) {
        if (!include_existing(get_body_position(state.dynamic_owners[i].id))) {
            return false;
        }
    }
    const auto count = horizon.count;
    // Everything that can refuse happens before publication. Staged shapes have fixed
    // slots and replace old references; steady ticks allocate nothing. Changed geometry
    // holds at most two sets of 128 shapes, independent of world size or tick count.
    for (std::size_t i = 0; i < count; ++i) {
        auto& staged = state.staged[i];
        staged.id = horizon.columns[i];
        const auto coords = Chunk::decode_id(staged.id);
        ++state.sampled_count;
        if (!m_world_system->sample_simulation_collision(coords.x, coords.z, staged.heights)) {
            return false;
        }
        const auto& published = state.published[i];
        if (i < state.published_count && staged.id == published.id &&
            std::equal(staged.heights.begin(),
                       staged.heights.end(),
                       published.heights.begin(),
                       [](float left, float right) {
                           return std::bit_cast<std::uint32_t>(left) ==
                                  std::bit_cast<std::uint32_t>(right);
                       })) {
            staged.shape = published.shape;
            continue;
        }
        JPH::HeightFieldShapeSettings settings(staged.heights.data(),
                                               JPH::Vec3::sZero(),
                                               JPH::Vec3::sReplicate(1.0f),
                                               CHUNK_SIZE_X + 1);
        settings.mBlockSize = 2;
        const auto result = settings.Create();
        if (result.HasError() || !result.Get()) {
            return false;
        }
        staged.shape = result.Get();
    }
    if (count != 0 && state.placeholder == nullptr) {
        state.placeholder = new JPH::SphereShape(0.01f);
    }
    // Reserve stable body ids once. These bodies remain outside the broadphase until
    // the entire horizon succeeds. A pool refusal retains already reserved slots.
    while (state.body_count < kSimulationColliderBudget && count != 0) {
        const JPH::BodyCreationSettings settings(state.placeholder.GetPtr(),
                                                 JPH::RVec3::sZero(),
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        auto* body = m_body_interface->CreateBody(settings);
        if (!body) {
            return false;
        }
        state.bodies[state.body_count++] = body->GetID();
        // Collider slots are body producers too: the allocator-history mirror must
        // match Jolt's free list or a restored owner set reuses the wrong BodyIDs.
        record_body_allocation(body->GetID());
    }
    // Publication cannot refuse. Physics waits on any failure above, on every host;
    // it never advances against a partially prepared horizon or render-only bodies.
    for (std::size_t i = 0; i < state.published_count; ++i) {
        if (i >= count || state.published[i].id != state.staged[i].id ||
            state.published[i].shape != state.staged[i].shape) {
            m_body_interface->RemoveBody(state.bodies[i]);
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (i < state.published_count && state.published[i].id == state.staged[i].id &&
            state.published[i].shape == state.staged[i].shape) {
            continue;
        }
        const auto coords = Chunk::decode_id(state.staged[i].id);
        m_body_interface->SetShape(
            state.bodies[i], state.staged[i].shape.GetPtr(), false, JPH::EActivation::DontActivate);
        m_body_interface->SetPosition(state.bodies[i],
                                      JPH::RVec3(static_cast<float>(coords.x * CHUNK_SIZE_X),
                                                 0.0f,
                                                 static_cast<float>(coords.z * CHUNK_SIZE_Z)),
                                      JPH::EActivation::DontActivate);
        m_body_interface->AddBody(state.bodies[i], JPH::EActivation::DontActivate);
        state.published[i] = state.staged[i];
    }
    for (std::size_t i = count; i < state.published_count; ++i) {
        m_body_interface->SetShape(
            state.bodies[i], state.placeholder.GetPtr(), false, JPH::EActivation::DontActivate);
        state.published[i].shape = nullptr;
    }
    for (auto& staged : state.staged) {
        staged.shape = nullptr;
    }
    state.published_count = count;
    state.horizon = horizon;
    state.tick = tick;
    state.ready = true;
    return true;
} catch (const std::exception&) {
    // Allocation/shape preparation failures have the same no-publication policy.
    return false;
}

// Every body producer uses this mirror. Ordinary allocation/release is O(1),
// with fixed storage bounded by Jolt's 10240 body slots; captures copy only on
// explicit requests between ticks. No allocator history enters tick hashes.
struct PhysicsSystem::BodySlotHistory {
    std::array<std::uint32_t, kPhysicsCapacity> free_slots{};
    std::uint32_t free_count = 0;
    std::uint32_t slot_count = 0;
};

void PhysicsSystem::record_body_allocation(JPH::BodyID id) {
    if (id.IsInvalid()) {
        return;
    }
    if (m_body_slots->free_count != 0) {
        --m_body_slots->free_count;
    } else {
        ++m_body_slots->slot_count;
    }
}

void PhysicsSystem::record_body_release(JPH::BodyID id) {
    m_body_slots->free_slots[m_body_slots->free_count++] = id.GetIndexAndSequenceNumber();
}

bool PhysicsSystem::capture_simulation_terrain(SimulationTerrainState& output) const {
    SimulationTerrainState staged;
    if (m_simulation_collisions) {
        const auto& state = *m_simulation_collisions;
        staged.bodies.reserve(state.body_count);
        staged.columns.reserve(state.published_count);
        for (std::size_t i = 0; i < state.body_count; ++i) {
            staged.bodies.push_back(state.bodies[i].GetIndexAndSequenceNumber());
        }
        for (std::size_t i = 0; i < state.published_count; ++i) {
            staged.columns.push_back(
                {.id = state.published[i].id, .heights = state.published[i].heights});
        }
    }
    output = std::move(staged);
    return true;
}

bool PhysicsSystem::restore_simulation_terrain(const SimulationTerrainState& input) {
    if (!m_body_interface || m_simulation_collisions ||
        input.bodies.size() > kSimulationColliderBudget ||
        input.columns.size() > input.bodies.size()) {
        return false;
    }
    if (input.bodies.empty()) {
        return true;
    }
    auto state = std::make_unique<SimulationCollisions>();
    state->placeholder = new JPH::SphereShape(0.01f);
    // Cold isolated-world restore: all sizes are bounded to 128 before allocation.
    // The destination is discarded on any failure; no live world observes staging.
    for (std::size_t i = 0; i < input.columns.size(); ++i) {
        const auto& column = input.columns[i];
        const auto coords = Chunk::decode_id(column.id);
        if (coords.y != 0 || Chunk::calculate_id(coords) != column.id ||
            (i != 0 && input.columns[i - 1].id >= column.id) ||
            !std::all_of(column.heights.begin(), column.heights.end(), [](float height) {
                return std::isfinite(height) && height >= -16777216.0f && height < 16777216.0f;
            })) {
            return false;
        }
        JPH::HeightFieldShapeSettings settings(column.heights.data(),
                                               JPH::Vec3::sZero(),
                                               JPH::Vec3::sReplicate(1.0f),
                                               CHUNK_SIZE_X + 1);
        settings.mBlockSize = 2;
        const auto shape = settings.Create();
        if (shape.HasError() || !shape.Get()) {
            return false;
        }
        state->published[i] = {.id = column.id, .heights = column.heights, .shape = shape.Get()};
    }
    for (std::size_t i = 0; i < input.bodies.size(); ++i) {
        const JPH::BodyID id(input.bodies[i]);
        if (id.IsInvalid() || id.GetIndex() >= kPhysicsCapacity ||
            (input.bodies[i] & JPH::BodyID::cBroadPhaseBit) != 0) {
            return false;
        }
        const bool published = i < input.columns.size();
        JPH::RVec3 position = JPH::RVec3::sZero();
        if (published) {
            const auto coords = Chunk::decode_id(input.columns[i].id);
            position = JPH::RVec3(static_cast<float>(coords.x) * CHUNK_SIZE_X,
                                  0.0f,
                                  static_cast<float>(coords.z) * CHUNK_SIZE_Z);
        }
        const JPH::BodyCreationSettings settings(published ? state->published[i].shape.GetPtr()
                                                           : state->placeholder.GetPtr(),
                                                 position,
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        auto* body = CreateRestoredBody(*m_body_interface, settings, id);
        if (!body) {
            return false;
        }
        m_body_slots->slot_count = std::max(m_body_slots->slot_count, id.GetIndex() + 1);
        if (published) {
            m_body_interface->AddBody(id, JPH::EActivation::DontActivate);
        }
        state->bodies[i] = id;
        ++state->body_count;
    }
    state->published_count = input.columns.size();
    m_simulation_collisions = std::move(state);
    return true;
}

PhysicsSystem::PhysicsSystem() = default;
PhysicsSystem::~PhysicsSystem() {
    shutdown();
}

void PhysicsSystem::startup() {
    if (m_started) {
        LUMINUMBRA_CORE_WARN("PhysicsSystem startup requested while already running.");
        return;
    }

    auto body_slots = std::make_unique<BodySlotHistory>();
    PendingPhysicsResources pending;
    pending.lease = std::make_unique<JoltRuntimeLease>();
    pending.temp = std::make_unique<TempAllocatorImpl>(10 * 1024 * 1024);
    pending.physics = std::make_unique<JPH::PhysicsSystem>();
    static BPLayerInterfaceImpl broad_phase_layer_interface;
    static ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    static ObjectLayerPairFilterImpl object_vs_object_layer_filter;
    pending.physics->Init(kPhysicsCapacity,
                          0,
                          kPhysicsCapacity,
                          kPhysicsCapacity,
                          broad_phase_layer_interface,
                          object_vs_broadphase_layer_filter,
                          object_vs_object_layer_filter);
    const uint hardware_threads = std::thread::hardware_concurrency();
    const uint num_worker_threads = hardware_threads > 1 ? hardware_threads - 1 : 1;
    pending.jobs = std::make_unique<JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers, num_worker_threads);
    auto* const body_interface = &pending.physics->GetBodyInterface();
    LUMINUMBRA_CORE_INFO("Jolt Physics System Initialized.");

    // Publication is nonthrowing; no borrowed interface escapes preparation.
    m_runtime_lease.swap(pending.lease);
    m_temp_allocator.swap(pending.temp);
    m_jolt_system.swap(pending.physics);
    m_jolt_job_system.swap(pending.jobs);
    m_body_interface = body_interface;
    m_body_slots.swap(body_slots);
    m_started = true;
}

void PhysicsSystem::shutdown() {
    if (!m_started) {
        return;
    }

    clear_simulation_collisions();
    m_simulation_collisions.reset();

    // Owner-thread lifecycle: no update or new producer may overlap shutdown.
    // Barrier completion alone does not release queue-owned job captures.
    m_jolt_job_system.reset();
    if (m_body_interface) {
        for (const auto& [chunk_id, collision] : m_chunk_bodies) {
            (void)chunk_id;
            m_body_interface->RemoveBody(collision.body_id);
            m_body_interface->DestroyBody(collision.body_id);
            record_body_release(collision.body_id);
        }
    }
    m_chunk_bodies.clear();
    m_player_character.reset();
    clear_avatar_characters(); // Release server avatars before the Jolt system.
    // Properly release the reference-counted shapes
    m_player_stand_shape = nullptr;
    m_player_crouch_shape = nullptr;
    m_avatar_shape = nullptr;
    m_body_interface = nullptr;
    m_jolt_system.reset();
    m_body_slots.reset();
    m_temp_allocator.reset();
    m_runtime_lease.reset();
    m_started = false;
}

void PhysicsSystem::update(float delta_time) {
    m_work_items = {};
    if (!m_jolt_system) {
        return;
    }
    m_work_items.bodies = non_simulation_body_count();
    if (!simulation_collisions_ready()) {
        return;
    }
    // Bodies beyond the bounded horizon wait with their velocities retained.
    // Other owners and session lifecycle work continue, allowing later admission.
    if (simulation_collisions_enabled() && m_simulation_collisions) {
        auto& state = *m_simulation_collisions;
        for (std::size_t i = 0; i < state.dynamic_count; ++i) {
            auto& owner = state.dynamic_owners[i];
            const bool covered = state.horizon.covers(get_body_position(owner.id));
            if (!covered && !owner.suspended) {
                owner.was_active = m_body_interface->IsActive(owner.id);
                m_body_interface->GetLinearAndAngularVelocity(
                    owner.id, owner.velocity, owner.angular_velocity);
                m_body_interface->SetMotionType(
                    owner.id, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
                m_body_interface->SetLinearAndAngularVelocity(
                    owner.id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
                m_body_interface->DeactivateBody(owner.id);
                owner.suspended = true;
            } else if (covered && owner.suspended) {
                m_body_interface->SetMotionType(
                    owner.id, JPH::EMotionType::Dynamic, JPH::EActivation::DontActivate);
                SetOwnerVelocityWithoutActivation(
                    *m_jolt_system, owner.id, owner.velocity, owner.angular_velocity);
                if (owner.was_active) {
                    m_body_interface->ActivateBody(owner.id);
                }
                owner.suspended = false;
            }
        }
    }
    m_jolt_system->Update(delta_time, 1, m_temp_allocator.get(), m_jolt_job_system.get());

    // Process batched physics queries each frame
    m_batched_queries.ProcessBatch(this);
}

std::optional<JPH::BodyID> PhysicsSystem::chunk_collision_body(ChunkID chunk_id) const {
    const auto found = m_chunk_bodies.find(chunk_id);
    if (found == m_chunk_bodies.end()) {
        return std::nullopt;
    }
    return found->second.body_id;
}

bool PhysicsSystem::add_chunk_collision(Chunk& chunk) {
    if (simulation_collisions_enabled()) {
        return true;
    }
    return add_chunk_collision(chunk, JPH::BodyID());
}

bool PhysicsSystem::capture_chunk_collision_owners(
    std::vector<std::pair<ChunkID, JPH::BodyID>>& output) const {
    if (m_chunk_bodies.size() > kTerrainBodyHeadroom) {
        return false;
    }
    std::vector<std::pair<ChunkID, JPH::BodyID>> candidate;
    candidate.reserve(m_chunk_bodies.size());
    for (const auto& [chunk_id, collision] : m_chunk_bodies) {
        candidate.emplace_back(chunk_id, collision.body_id);
    }
    std::sort(candidate.begin(), candidate.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first < rhs.first;
    });
    output = std::move(candidate);
    return true;
}

bool PhysicsSystem::add_chunk_collision(Chunk& chunk, JPH::BodyID restored_id) {
    if (simulation_collisions_enabled()) {
        return true; // Render/warm-up/edit calls cannot mutate simulation's private bodies.
    }
    if (!m_body_interface)
        return true;
    if (m_chunk_bodies.find(chunk.get_id()) != m_chunk_bodies.end())
        return true;

    constexpr std::size_t kHeightmapSide = CHUNK_SIZE_X + 1;
    if (chunk.heightmap_data.size() != kHeightmapSide * kHeightmapSide) {
        LUMINUMBRA_CORE_WARN("Attempted to add chunk collision for chunk ({}, {}, {}) with "
                             "incomplete heightmap data.",
                             chunk.get_coords().x,
                             chunk.get_coords().y,
                             chunk.get_coords().z);
        return true;
    }

    // 1. Copy the cached heightmap data into a Jolt-compatible array.
    // The resolution of our heightmap includes the +1 padding for seamless chunk borders.
    const int resolution = CHUNK_SIZE_X + 1;
    JPH::Array<float> height_samples;
    height_samples.resize(resolution * resolution);

    float min_h = 99999.0f;
    float max_h = -99999.0f;

    for (int i = 0; i < resolution * resolution; ++i) {
        float h = chunk.heightmap_data[static_cast<std::size_t>(i)];
        height_samples[static_cast<std::size_t>(i)] = h;
        if (h < min_h)
            min_h = h;
        if (h > max_h)
            max_h = h;
    }

    glm::ivec3 cc = chunk.get_coords();
    const float chunk_min_y = static_cast<float>(cc.y) * static_cast<float>(CHUNK_SIZE_Y);
    const float chunk_max_y = static_cast<float>(cc.y + 1) * static_cast<float>(CHUNK_SIZE_Y);
    const int center_index = (CHUNK_SIZE_Z / 2) * resolution + (CHUNK_SIZE_X / 2);
    const float center_height = chunk.heightmap_data[static_cast<std::size_t>(center_index)];
    if (center_height < chunk_min_y || center_height >= chunk_max_y) {
        return true;
    }

    // 2. Create the HeightFieldShape.
    // The height samples are in absolute world coordinates. We provide Jolt with the
    // world-space position of the heightmap's origin (the corner of the chunk).
    glm::vec3 chunk_base_pos(cc.x * CHUNK_SIZE_X, 0.0f, cc.z * CHUNK_SIZE_Z);
    JPH::HeightFieldShapeSettings shape_settings(
        height_samples.data(),
        JPH::Vec3(chunk_base_pos.x, 0.0f, chunk_base_pos.z),
        JPH::Vec3(1.0f, 1.0f, 1.0f),
        resolution);
    shape_settings.mBlockSize = 2; // Recommended default for good performance

    JPH::ShapeSettings::ShapeResult result = shape_settings.Create();
    if (result.HasError()) {
        LUMINUMBRA_CORE_ERROR(
            "HeightFieldShape error for chunk ({},{},{}): {}", cc.x, cc.y, cc.z, result.GetError());
        return true;
    }

    // Additional safety check
    if (!result.Get()) {
        LUMINUMBRA_CORE_ERROR(
            "HeightFieldShape creation returned null for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return true;
    }

    // The position is baked into the shape, so the body itself can be created at the world origin.
    JPH::BodyCreationSettings body_settings(result.Get(),
                                            JPH::RVec3::sZero(),
                                            JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static,
                                            Layers::NON_MOVING);
    JPH::Body* body = restored_id.IsInvalid()
                          ? m_body_interface->CreateBody(body_settings)
                          : CreateRestoredBody(*m_body_interface, body_settings, restored_id);
    if (body == nullptr) {
        LUMINUMBRA_CORE_WARN(
            "Body pool refused chunk collision for chunk ({},{},{})", cc.x, cc.y, cc.z);
        return false;
    }
    if (restored_id.IsInvalid()) {
        record_body_allocation(body->GetID());
    } else {
        m_body_slots->slot_count = std::max(m_body_slots->slot_count, restored_id.GetIndex() + 1);
    }
    m_body_interface->AddBody(body->GetID(), JPH::EActivation::DontActivate);
    m_chunk_bodies[chunk.get_id()] = ChunkCollisionData{body->GetID()};
    return true;
}

void PhysicsSystem::remove_chunk_collision(ChunkID id) {
    if (simulation_collisions_enabled()) {
        return;
    }
    if (!m_body_interface)
        return;
    auto it = m_chunk_bodies.find(id);
    if (it == m_chunk_bodies.end())
        return;
    m_body_interface->RemoveBody(it->second.body_id);
    m_body_interface->DestroyBody(it->second.body_id);
    record_body_release(it->second.body_id);
    m_chunk_bodies.erase(it);
}

bool PhysicsSystem::create_player_controller(const glm::vec3& start_pos) {
    if (!m_jolt_system || !can_admit_simulation_owner(start_pos, true)) {
        return false;
    }

    const auto standing = FeetCapsule(0.9f, 1.3f);
    const auto crouching = FeetCapsule(0.45f, 0.85f);
    auto character = PrepareController(standing, start_pos, m_jolt_system.get());
    m_player_stand_shape = standing;
    m_player_crouch_shape = crouching;
    m_player_character = std::move(character);
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    LUMINUMBRA_CORE_INFO("Player controller created.");
    return true;
}

void PhysicsSystem::update_player(const glm::vec3& wish_velocity,
                                  bool wants_to_jump,
                                  float jump_force,
                                  float dt) {
    if (!m_player_character || !m_jolt_system)
        return;
    // A teleport can occur inside the input driver after the host's frame barrier.
    // Prepare its destination before any character velocity or position changes.
    if (!simulation_collisions_ready() && !prepare_simulation_collisions({})) {
        return;
    }

    if (simulation_collisions_enabled() && m_simulation_collisions &&
        !m_simulation_collisions->horizon.covers(get_player_position())) {
        return;
    }
    ++m_work_items.characters;

    // Get the character's current state
    JPH::Vec3 current_velocity = m_player_character->GetLinearVelocity();
    bool is_grounded =
        m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    // Start with the desired horizontal velocity from player input.
    // The vertical component will be calculated next.
    JPH::Vec3 desired_velocity(wish_velocity.x, current_velocity.GetY(), wish_velocity.z);

    if (is_grounded) {
        // The character is on the ground.
        if (wants_to_jump) {
            // Apply jump impulse to the vertical velocity.
            desired_velocity.SetY(jump_force);
        } else {
            // When on the ground and not jumping, reset vertical velocity.
            // A small negative value helps the character "stick" to slopes.
            desired_velocity.SetY(-1.0f);
        }
    } else {
        // The character is in the air. Manually apply gravity.
        // Jolt's Update function does NOT do this for us.
        JPH::Vec3 gravity = m_jolt_system->GetGravity();
        desired_velocity.SetY(current_velocity.GetY() + gravity.GetY() * dt);
    }

    // Set the calculated velocity on the character controller.
    m_player_character->SetLinearVelocity(desired_velocity);

    // Update the character controller, which will handle movement and collision.
    // The gravity parameter here is used for ground detection, not for applying acceleration.
    //
    // TRAVERSAL-LITE step-up / mantle: use Jolt's ExtendedUpdate
    // instead of the plain Update. ExtendedUpdate runs WalkStairs internally, which
    // robustly lifts the capsule over short ledges (up to mWalkStairsStepUp high)
    // when forward motion is blocked by a low obstacle that has clear space above --
    // exactly the "step up a ledge without jumping" behaviour we want. This is NOT a
    // climb FSM and NOT wall-scaling: mMaxSlopeAngle still blocks steep faces taller
    // than the step, and the lift only triggers when there's horizontal wish motion.
    //
    // All settings are FIXED constants (no wall-clock / no random) so the path stays
    // deterministic. NOTE: this is the CLIENT-ONLY local player controller; the
    // server-authoritative avatars (update_avatars, which fold into the entities
    // sub-hash) are deliberately left on plain Update -> no world_hash change.
    JPH::CharacterVirtual::ExtendedUpdateSettings step_settings;
    // Lift over ledges up to ~0.7 m (knee/waist height) -- mounts short steps and
    // small ledges without a jump. Jolt default is 0.4 m; we raise it for traversal.
    step_settings.mWalkStairsStepUp = JPH::Vec3(0.0f, 0.7f, 0.0f);
    // Match the step-down sweep so the character settles back onto the surface after
    // cresting a step (otherwise it can briefly float off the top of the ledge).
    step_settings.mWalkStairsStepDownExtra = JPH::Vec3(0.0f, -0.7f, 0.0f);
    // Keep the default stick-to-floor sweep so it tracks down the far side of a step.
    // (mStickToFloorStepDown left at its default of {0,-0.5,0}.)
    m_player_character->ExtendedUpdate(dt,
                                       m_jolt_system->GetGravity(),
                                       step_settings,
                                       BroadPhaseLayerFilterAll(),
                                       ObjectLayerFilterAll(),
                                       JPH::BodyFilter(),
                                       JPH::ShapeFilter(),
                                       *m_temp_allocator);
}

bool PhysicsSystem::set_player_crouched(bool is_crouched) {
    if (!m_player_character || !m_jolt_system)
        return false;

    // --- REFACTORED: Use cached shapes to avoid memory leaks/reallocation ---
    JPH::Ref<JPH::Shape> target_shape = is_crouched ? m_player_crouch_shape : m_player_stand_shape;

    // Only change the shape if it's actually different
    if (m_player_character->GetShape() != target_shape) {
        return m_player_character->SetShape(
            target_shape,
            1.5f * m_jolt_system->GetPhysicsSettings().mPenetrationSlop,
            BroadPhaseLayerFilterAll(),
            ObjectLayerFilterAll(),
            JPH::BodyFilter(),
            JPH::ShapeFilter(),
            *m_temp_allocator);
    }
    return true;
}

void PhysicsSystem::set_player_position(const glm::vec3& position) {
    if (m_player_character) {
        m_player_character->SetPosition(JPH::RVec3(position.x, position.y, position.z));
        if (m_simulation_collisions) {
            m_simulation_collisions->ready = false;
        }
    }
}

glm::vec3 PhysicsSystem::get_player_position() const {
    if (!m_player_character)
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_player_character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

bool PhysicsSystem::is_player_grounded() const {
    if (!m_player_character)
        return false;
    return m_player_character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

// ---  : dynamic rigid-body projectiles ---
JPH::BodyID PhysicsSystem::create_dynamic_sphere(const glm::vec3& position,
                                                 const glm::vec3& velocity,
                                                 float radius) {
    if (!m_body_interface)
        return JPH::BodyID();
    // Register every sphere, even while terrain collisions are off, matching
    // restore: a later durable-residency activation covers the same owners in
    // the same order on an original and a restored continuation.
    if (!m_simulation_collisions) {
        m_simulation_collisions = std::make_unique<SimulationCollisions>();
    }
    if (m_simulation_collisions->dynamic_count == m_simulation_collisions->dynamic_owners.size()) {
        return JPH::BodyID();
    }
    JPH::BodyCreationSettings settings(new JPH::SphereShape(radius),
                                       JPH::RVec3(position.x, position.y, position.z),
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic,
                                       Layers::MOVING);
    settings.mLinearVelocity = JPH::Vec3(velocity.x, velocity.y, velocity.z);
    // A light, lively projectile: low gravity factor would float it; keep 1.0 so it
    // arcs naturally and rests on the terrain. Continuous collision avoids tunnelling
    // through thin geometry at speed.
    settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
    const JPH::BodyID id = m_body_interface->CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (!id.IsInvalid()) {
        auto& state = *m_simulation_collisions;
        state.dynamic_owners[state.dynamic_count++] = {.id = id,
                                                       .velocity = JPH::Vec3::sZero(),
                                                       .angular_velocity = JPH::Vec3::sZero(),
                                                       .suspended = false,
                                                       .was_active = false};
        state.dynamic_slots[id.GetIndex()] = static_cast<std::uint32_t>(state.dynamic_count);
        state.ready = false;
    }
    record_body_allocation(id);
    return id;
}

glm::vec3 PhysicsSystem::get_body_position(JPH::BodyID body) const {
    if (!m_body_interface || body.IsInvalid())
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_body_interface->GetPosition(body);
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

bool PhysicsSystem::body_is_active(JPH::BodyID body) const {
    if (!m_body_interface || body.IsInvalid())
        return false;
    if (m_simulation_collisions &&
        body.GetIndex() < m_simulation_collisions->dynamic_slots.size()) {
        const auto& state = *m_simulation_collisions;
        const auto slot = state.dynamic_slots[body.GetIndex()];
        if (slot != 0) {
            const auto& owner = state.dynamic_owners[slot - 1];
            if (owner.id == body && owner.suspended) {
                return owner.was_active; // Waiting for terrain is not projectile rest.
            }
        }
    }
    return m_body_interface->IsActive(body);
}

void PhysicsSystem::destroy_body(JPH::BodyID body) {
    if (!m_body_interface || body.IsInvalid())
        return;
    m_body_interface->RemoveBody(body);
    m_body_interface->DestroyBody(body);
    if (m_simulation_collisions) {
        auto& state = *m_simulation_collisions;
        if (body.GetIndex() < state.dynamic_slots.size()) {
            const auto slot = state.dynamic_slots[body.GetIndex()];
            if (slot != 0 && state.dynamic_owners[slot - 1].id == body) {
                state.dynamic_slots[body.GetIndex()] = 0;
                const auto index = slot - 1;
                state.dynamic_owners[index] = state.dynamic_owners[--state.dynamic_count];
                if (index < state.dynamic_count) {
                    state.dynamic_slots[state.dynamic_owners[index].id.GetIndex()] = slot;
                }
            }
        }
    }
    record_body_release(body);
}

// ---  : server-authoritative avatar characters ---
void PhysicsSystem::clear_avatar_characters() {
    for (const auto& avatar : m_avatars) {
        destroy_body(avatar.frozen_body);
    }
    m_avatars.clear();
    m_free_avatar_slots.clear();
    m_avatar_head = kNoAvatar;
    m_avatar_tail = kNoAvatar;
    m_live_avatar_count = 0;
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
}

void PhysicsSystem::remove_avatar_character(std::size_t index) {
    if (index >= m_avatars.size() || !m_avatars[index].character) {
        return;
    }
    const auto previous = m_avatars[index].previous;
    const auto next = m_avatars[index].next;
    if (previous != kNoAvatar) {
        m_avatars[previous].next = next;
    } else {
        m_avatar_head = next;
    }
    if (next != kNoAvatar) {
        m_avatars[next].previous = previous;
    } else {
        m_avatar_tail = previous;
    }
    --m_live_avatar_count;
    destroy_body(m_avatars[index].frozen_body);
    m_avatars[index] = AvatarState{};
    m_free_avatar_slots.push_back(index);
    std::push_heap(
        m_free_avatar_slots.begin(), m_free_avatar_slots.end(), std::greater<std::size_t>{});
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
}

void PhysicsSystem::set_avatar_frozen(std::size_t index, bool frozen, const glm::vec3& position) {
    if (index >= m_avatars.size() || !m_avatars[index].character) {
        return;
    }
    auto& avatar = m_avatars[index];
    if (!frozen && !avatar.frozen) {
        return;
    }
    auto& body = avatar.frozen_body;
    auto& character = *avatar.character;
    const JPH::RVec3 feet =
        std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z)
            ? JPH::RVec3(position.x, position.y, position.z)
            : character.GetPosition();
    if (frozen && body.IsInvalid()) {
        const JPH::BodyCreationSettings settings(m_avatar_shape,
                                                 feet,
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        // Count admission is O(1), in the caller's existing entity order. Leave
        // terrain headroom even after this capsule takes a slot. Both headroom
        // refusal and a null CreateBody leave an invalid ID for the next retry.
        const auto max_bodies = m_jolt_system->GetMaxBodies();
        if (max_bodies > kTerrainBodyHeadroom &&
            non_simulation_body_count() < max_bodies - kTerrainBodyHeadroom) {
            body = m_body_interface->CreateAndAddBody(settings, JPH::EActivation::DontActivate);
            record_body_allocation(body);
        }
    } else if (!frozen) {
        destroy_body(body);
        body = JPH::BodyID();
    } else if (feet != character.GetPosition()) {
        m_body_interface->SetPosition(body, feet, JPH::EActivation::DontActivate);
    }
    avatar.frozen = frozen;
    character.SetPosition(feet);
    character.SetLinearVelocity(JPH::Vec3::sZero());
    avatar.wish = glm::vec2(0.0f);
}

void PhysicsSystem::set_avatar_wish_velocity(std::size_t index, const glm::vec2& wish_xz) {
    if (index < m_avatars.size() && m_avatars[index].character) {
        m_avatars[index].wish = m_avatars[index].frozen ? glm::vec2(0.0f) : wish_xz;
    }
}

std::size_t PhysicsSystem::create_avatar_character(const glm::vec3& start_pos) {
    if (!m_jolt_system || !can_admit_simulation_owner(start_pos, false)) {
        return kInvalidAvatar;
    }
    // Shared capsule (same dimensions as the standing player) created once.
    const auto shape = m_avatar_shape != nullptr ? m_avatar_shape : FeetCapsule(0.9f, 1.3f);
    std::vector<std::size_t> retirement_storage;
    if (m_free_avatar_slots.empty() && m_free_avatar_slots.capacity() <= m_avatars.size()) {
        // Stage geometric growth so a refused character/append retains no new
        // storage. Cap before addition to avoid size overflow.
        const auto capacity = m_free_avatar_slots.capacity();
        const auto maximum = m_free_avatar_slots.max_size();
        if (m_avatars.size() == maximum) {
            throw std::length_error("Avatar slot capacity exhausted");
        }
        retirement_storage.reserve(
            std::max(m_avatars.size() + 1, capacity + std::min(capacity, maximum - capacity)));
    }
    AvatarState avatar{.character = PrepareController(shape, start_pos, m_jolt_system.get()),
                       .wish = glm::vec2(0.0f),
                       .frozen_body = JPH::BodyID(),
                       .frozen = false,
                       .previous = m_avatar_tail,
                       .next = kNoAvatar};
    auto index = m_avatars.size();
    if (!m_free_avatar_slots.empty()) {
        index = m_free_avatar_slots.front();
        m_avatars[index] = std::move(avatar);
        std::pop_heap(
            m_free_avatar_slots.begin(), m_free_avatar_slots.end(), std::greater<std::size_t>{});
        m_free_avatar_slots.pop_back();
    } else {
        m_avatars.push_back(std::move(avatar));
        if (retirement_storage.capacity() != 0) {
            m_free_avatar_slots.swap(retirement_storage);
        }
    }
    m_avatar_shape = shape;
    // Publish both append and reuse only after every throwing operation succeeds.
    // Admission, collision preparation and stepping all use this creation order.
    if (m_avatar_tail != kNoAvatar) {
        m_avatars[m_avatar_tail].next = index;
    } else {
        m_avatar_head = index;
    }
    m_avatar_tail = index;
    ++m_live_avatar_count;
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    return index;
}

PhysicsSystem::AvatarCharacterDiagnostics PhysicsSystem::avatar_character_diagnostics() const {
    AvatarCharacterDiagnostics result{.live_count = m_live_avatar_count, .creation_order = {}};
    // Explicit diagnostics only: at most the peak live slot count, never a tick pass.
    result.creation_order.reserve(m_live_avatar_count);
    for (auto index = m_avatar_head; index != kNoAvatar; index = m_avatars[index].next) {
        result.creation_order.push_back(index);
    }
    return result;
}

void PhysicsSystem::update_avatars(float dt) {
    if (!m_jolt_system || !simulation_collisions_ready())
        return;
    // Step the live list in creation order, including reused slots at the tail,
    // so collide-and-slide remains deterministic for the same lifecycle history.
    // Gravity and ground-stick mirror update_player's vertical handling.
    const JPH::Vec3 gravity = m_jolt_system->GetGravity();
    for (auto i = m_avatar_head; i != kNoAvatar; i = m_avatars[i].next) {
        auto& character = m_avatars[i].character;
        if (!character || m_avatars[i].frozen ||
            (simulation_collisions_enabled() && m_simulation_collisions &&
             !m_simulation_collisions->horizon.covers(get_avatar_position(i)))) {
            continue;
        }
        ++m_work_items.characters;
        const glm::vec2 wish = m_avatars[i].wish;
        const JPH::Vec3 current_velocity = character->GetLinearVelocity();
        const bool grounded =
            character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        JPH::Vec3 desired_velocity(wish.x, current_velocity.GetY(), wish.y);
        if (grounded) {
            desired_velocity.SetY(-1.0f); // stick to slopes / ground
        } else {
            desired_velocity.SetY(current_velocity.GetY() + gravity.GetY() * dt);
        }
        character->SetLinearVelocity(desired_velocity);
        character->Update(dt,
                          gravity,
                          BroadPhaseLayerFilterAll(),
                          ObjectLayerFilterAll(),
                          JPH::BodyFilter(),
                          JPH::ShapeFilter(),
                          *m_temp_allocator);
    }
}

glm::vec3 PhysicsSystem::get_avatar_position(std::size_t index) const {
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return glm::vec3(0.0f);
    const JPH::RVec3 p = m_avatars[index].character->GetPosition();
    return glm::vec3((float)p.GetX(), (float)p.GetY(), (float)p.GetZ());
}

glm::vec3 PhysicsSystem::get_avatar_velocity(std::size_t index) const {
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return glm::vec3(0.0f);
    const JPH::Vec3 v = m_avatars[index].character->GetLinearVelocity();
    return glm::vec3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ());
}

bool PhysicsSystem::is_avatar_grounded(std::size_t index) const {
    if (index >= m_avatars.size() || !m_avatars[index].character)
        return false;
    return m_avatars[index].character->GetGroundState() ==
           JPH::CharacterBase::EGroundState::OnGround;
}

bool PhysicsSystem::capture_player_owner(World::PhysicsOwnerState& output) const {
    if (!m_player_character) {
        return false;
    }
    OwnerRecipe recipe;
    recipe.kind = 2u;
    recipe.flags = m_player_character->GetShape() == m_player_crouch_shape ? 2u : 0u;
    try {
        m_player_character->SaveState(recipe.owner);
        return WriteController(recipe, output);
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool PhysicsSystem::restore_player_owner(const World::PhysicsOwnerState& state) {
    OwnerRecipe recipe;
    if (!m_jolt_system || m_player_character || !ParseController(state, recipe) ||
        recipe.kind != 2u ||
        !SimulationHorizon::valid(Vec3(static_cast<float>(recipe.feet.GetX()),
                                       static_cast<float>(recipe.feet.GetY()),
                                       static_cast<float>(recipe.feet.GetZ()))) ||
        !ReferencesExist(recipe, *m_jolt_system, std::span<const std::uint32_t>{})) {
        return false;
    }
    // Stage both stances and all saved contacts before publishing the singleton.
    const auto standing = FeetCapsule(0.9f, 1.3f);
    const auto crouching = FeetCapsule(0.45f, 0.85f);
    auto character = PrepareController(
        recipe.flags == 2u ? crouching : standing, glm::vec3(0.0f), m_jolt_system.get());
    character->RestoreState(recipe.owner);
    m_player_stand_shape = standing;
    m_player_crouch_shape = crouching;
    m_player_character = std::move(character);
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    return true;
}

bool PhysicsSystem::capture_avatar_owner(std::size_t index,
                                         World::PhysicsOwnerState& output) const {
    if (!m_jolt_system || index >= m_avatars.size() || !m_avatars[index].character) {
        return false;
    }
    const auto& avatar = m_avatars[index];
    OwnerRecipe recipe;
    recipe.kind = 1u;
    recipe.flags = avatar.frozen ? 1u : 0u;
    recipe.wish = avatar.wish;
    recipe.body = avatar.frozen_body.GetIndexAndSequenceNumber();
    if (!avatar.frozen_body.IsInvalid()) {
        recipe.slot_count = m_body_slots->slot_count;
        recipe.free_slots.assign(m_body_slots->free_slots.begin(),
                                 m_body_slots->free_slots.begin() + m_body_slots->free_count);
    }
    try {
        avatar.character->SaveState(recipe.owner);
        return WriteController(recipe, output);
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool PhysicsSystem::restore_avatar_owner(
    const World::PhysicsOwnerState& state,
    std::size_t& index,
    std::span<const World::PhysicsOwnerState> remaining_frozen) {
    OwnerRecipe recipe;
    if (!m_jolt_system || m_avatars.size() >= kMaxRestoredAvatars ||
        !ParseController(state, recipe) || recipe.kind != 1u ||
        !SimulationHorizon::valid(Vec3(static_cast<float>(recipe.feet.GetX()),
                                       static_cast<float>(recipe.feet.GetY()),
                                       static_cast<float>(recipe.feet.GetZ()))) ||
        remaining_frozen.size() >= kMaxRestoredAvatars - m_avatars.size()) {
        return false;
    }
    // The codec carries a global allocator, but each controller owns one slot.
    // At most 4095 remaining captures, each bounded to 64 KiB,
    // may explain slots absent from the destination. Never infer reservations
    // from unexplained holes: malformed, duplicate and conflicting IDs refuse.
    std::vector<std::uint32_t> reserved;
    reserved.reserve(remaining_frozen.size());
    for (const auto& pending : remaining_frozen) {
        OwnerRecipe reservation;
        if (!ParseController(pending, reservation) || reservation.kind != 1u ||
            reservation.flags != 1u || reservation.body == JPH::BodyID::cInvalidBodyID ||
            !SimulationHorizon::valid(Vec3(static_cast<float>(reservation.feet.GetX()),
                                           static_cast<float>(reservation.feet.GetY()),
                                           static_cast<float>(reservation.feet.GetZ())))) {
            return false;
        }
        // Live controllers have no allocator trailer. The first pending capsule
        // supplies it; all other captures must describe that same exact topology.
        if (recipe.body == JPH::BodyID::cInvalidBodyID && reserved.empty()) {
            recipe.slot_count = reservation.slot_count;
            recipe.free_slots = reservation.free_slots;
        }
        if (reservation.slot_count != recipe.slot_count ||
            reservation.free_slots != recipe.free_slots) {
            return false;
        }
        reserved.push_back(reservation.body);
    }
    bool prepared_capsule = false;
    JPH::BodyIDVector existing;
    if (recipe.body != JPH::BodyID::cInvalidBodyID || !reserved.empty()) {
        m_jolt_system->GetBodies(existing);
    }
    if (recipe.body != JPH::BodyID::cInvalidBodyID) {
        const JPH::BodyID id(recipe.body);
        for (const auto occupied : existing) {
            if (occupied.GetIndex() == id.GetIndex() && occupied != id) {
                return false;
            }
        }
        const JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(), id);
        if (lock.Succeeded()) {
            // Jolt stores the centre of mass; GetPosition() subtracts the capsule's
            // offset again and is not bit-exact. Compare the stored value with the
            // same expression Jolt used to create or move the capsule at the feet.
            if (!lock.GetBody().IsStatic() || m_avatar_shape == nullptr ||
                lock.GetBody().GetShape() != m_avatar_shape ||
                lock.GetBody().GetCenterOfMassPosition() !=
                    recipe.feet + JPH::Quat::sIdentity() * m_avatar_shape->GetCenterOfMass() ||
                lock.GetBody().GetRotation() != JPH::Quat::sIdentity()) {
                return false;
            }
            for (const auto& owned : m_avatars) {
                if (owned.frozen_body == id) {
                    return false;
                }
            }
            prepared_capsule = true;
        }
    }
    if (prepared_capsule && (recipe.slot_count != m_body_slots->slot_count ||
                             recipe.free_slots.size() != m_body_slots->free_count ||
                             !std::equal(recipe.free_slots.begin(),
                                         recipe.free_slots.end(),
                                         m_body_slots->free_slots.begin()))) {
        return false;
    }
    const bool needs_capsule = recipe.body != JPH::BodyID::cInvalidBodyID && !prepared_capsule;
    BodySetState allocator;
    if (needs_capsule || !reserved.empty()) {
        allocator.slot_count = recipe.slot_count;
        allocator.free_slots = recipe.free_slots;
        if (!ValidateOwnerAllocator(allocator,
                                    needs_capsule ? std::span(&recipe, 1)
                                                  : std::span<const OwnerRecipe>{},
                                    existing,
                                    m_body_slots->slot_count,
                                    reserved)) {
            return false;
        }
    }

    // Later frozen owners may supply contact bodies, but only after their exact
    // IDs and the complete allocator topology have passed admission.
    if (!ReferencesExist(recipe, *m_jolt_system, reserved)) {
        return false;
    }

    auto scratch = PrepareOwnerAllocatorScratch(
        *m_body_interface, needs_capsule && !recipe.free_slots.empty(), Layers::NON_MOVING);
    if (needs_capsule && !recipe.free_slots.empty() && !scratch) {
        return false;
    }
    // Shapes, controller, contacts and the unassigned capsule are staged under
    // RAII. Broadphase preparation also rolls back before any avatar is published.
    m_avatars.reserve(m_avatars.size() + 1);
    // Removal must stay allocation-free: reserve its retirement slot for every
    // appended avatar before publication, as create_avatar_character does.
    m_free_avatar_slots.reserve(m_avatars.size() + 1);
    const auto shape = m_avatar_shape != nullptr ? m_avatar_shape : FeetCapsule(0.9f, 1.3f);
    AvatarState staged{.character = PrepareController(shape, glm::vec3(0.0f), m_jolt_system.get()),
                       .wish = recipe.wish,
                       .frozen_body = JPH::BodyID(),
                       .frozen = recipe.flags == 1u};
    staged.character->RestoreState(recipe.owner);
    UnassignedOwnerBody capsule(nullptr, UnassignedOwnerBodyDeleter{m_body_interface});
    if (needs_capsule) {
        const JPH::BodyCreationSettings settings(shape,
                                                 staged.character->GetPosition(),
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        capsule.reset(m_body_interface->CreateBodyWithoutID(settings));
        if (!capsule) {
            return false;
        }
    }
    if (needs_capsule) {
        JPH::BodyID id(recipe.body);
        JPH::BodyInterface::AddState addition = nullptr;
        try {
            AssignRestoredOwnerBody(*m_body_interface, capsule.get(), id);
            addition = m_body_interface->AddBodiesPrepare(&id, 1);
            m_body_interface->AddBodiesFinalize(&id, 1, addition, JPH::EActivation::DontActivate);
            addition = nullptr;
        } catch (...) {
            if (addition != nullptr) {
                m_body_interface->AddBodiesAbort(&id, 1, addition);
            }
            if (!capsule->GetID().IsInvalid()) {
                m_body_interface->UnassignBodyID(capsule->GetID());
            }
            // Explicit assignment can extend Jolt's pool, whose physical extent
            // cannot shrink. Reset new slots to generation zero in ascending
            // reuse order, making their next allocation identical to fresh slots.
            // Then restore every previously free generation and its stack order.
            // At most 10240 slots and 256 sequence increments per slot are used.
            for (auto extent = std::max(m_body_slots->slot_count, id.GetIndex() + 1);
                 extent > m_body_slots->slot_count;
                 --extent) {
                AssignRestoredOwnerBody(
                    *m_body_interface, capsule.get(), JPH::BodyID(extent - 1, 0));
                m_body_interface->UnassignBodyID(capsule->GetID());
            }
            RestoreOwnerFreeSlots(
                *m_body_interface,
                capsule.get(),
                std::span(m_body_slots->free_slots).first(m_body_slots->free_count));
            throw;
        }
        capsule.release();
        RestoreOwnerFreeSlots(*m_body_interface, scratch.get(), recipe.free_slots);
        m_body_slots->slot_count = recipe.slot_count;
        m_body_slots->free_count = static_cast<std::uint32_t>(recipe.free_slots.size());
        std::copy(
            recipe.free_slots.begin(), recipe.free_slots.end(), m_body_slots->free_slots.begin());
    }
    staged.frozen_body = JPH::BodyID(recipe.body);
    staged.previous = m_avatar_tail;
    staged.next = kNoAvatar;
    m_avatar_shape = shape;
    const auto slot = m_avatars.size();
    m_avatars.push_back(std::move(staged));
    if (m_avatar_tail != kNoAvatar) {
        m_avatars[m_avatar_tail].next = slot;
    } else {
        m_avatar_head = slot;
    }
    m_avatar_tail = slot;
    ++m_live_avatar_count;
    if (m_simulation_collisions) {
        m_simulation_collisions->ready = false;
    }
    index = slot;
    return true;
}

bool PhysicsSystem::OwnerContinuation::capture_avatar_owner(
    std::size_t index, World::PhysicsOwnerState& output) const {
    return m_physics.capture_avatar_owner(index, output);
}
bool PhysicsSystem::OwnerContinuation::restore_avatar_owner(
    const World::PhysicsOwnerState& state,
    std::size_t& index,
    std::span<const World::PhysicsOwnerState> remaining_frozen) {
    return m_physics.restore_avatar_owner(state, index, remaining_frozen);
}
bool PhysicsSystem::OwnerContinuation::capture_player_owner(
    World::PhysicsOwnerState& output) const {
    return m_physics.capture_player_owner(output);
}
bool PhysicsSystem::OwnerContinuation::restore_player_owner(const World::PhysicsOwnerState& state) {
    return m_physics.restore_player_owner(state);
}

bool PhysicsSystem::capture_body_owner(JPH::BodyID body, BodyOwnerState& output) const {
    if (!m_jolt_system || body.IsInvalid() || body.GetIndex() >= kPhysicsCapacity) {
        return false;
    }
    BodyOwnerState captured;
    if (m_simulation_collisions) {
        const auto& registrations = *m_simulation_collisions;
        const auto slot = registrations.dynamic_slots[body.GetIndex()];
        if (slot != 0) {
            const auto& registered = registrations.dynamic_owners[slot - 1];
            if (registered.id == body) {
                captured.registration_slot = slot;
            }
            if (registered.id == body && registered.suspended) {
                captured.suspended = true;
                captured.retained_velocity = {registered.velocity.GetX(),
                                              registered.velocity.GetY(),
                                              registered.velocity.GetZ()};
                captured.retained_angular_velocity = {registered.angular_velocity.GetX(),
                                                      registered.angular_velocity.GetY(),
                                                      registered.angular_velocity.GetZ()};
                captured.was_active = registered.was_active;
            }
        }
    }
    OwnerRecipe recipe;
    recipe.body = body.GetIndexAndSequenceNumber();
    try {
        {
            const JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(), body);
            if (!lock.Succeeded()) {
                return false;
            }
            const auto& owner = lock.GetBody();
            if ((owner.IsDynamic() || (owner.IsKinematic() && captured.suspended)) &&
                owner.GetShape()->GetSubType() == JPH::EShapeSubType::Sphere) {
                recipe.kind = kSphereOwner;
                recipe.radius = static_cast<const JPH::SphereShape*>(owner.GetShape())->GetRadius();
                m_jolt_system->SaveBodyState(owner, recipe.owner);
            } else if (owner.IsStatic() && m_avatar_shape != nullptr &&
                       owner.GetShape() == m_avatar_shape.GetPtr()) {
                recipe.kind = kFrozenBodyOwner;
                recipe.owner.Write(owner.GetPosition());
            } else {
                return false;
            }
        }
        if (recipe.kind == kSphereOwner) {
            const OwnerContactFilter filter(body, *m_jolt_system);
            // Jolt scans at most 10240 cached pairs on explicit capture only.
            m_jolt_system->SaveState(recipe.scene,
                                     JPH::EStateRecorderState::Global |
                                         JPH::EStateRecorderState::Contacts,
                                     &filter);
        }
        BodyOwnerState payload;
        if (!WriteOwner(recipe, payload)) {
            return false;
        }
        payload.suspended = captured.suspended;
        payload.retained_velocity = captured.retained_velocity;
        payload.retained_angular_velocity = captured.retained_angular_velocity;
        payload.was_active = captured.was_active;
        payload.registration_slot = captured.registration_slot;
        output = payload;
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool PhysicsSystem::capture_body_owners(BodySetState& output) const {
    if (!m_jolt_system || m_avatars.size() > kMaxRestoredAvatars) {
        return false;
    }
    // Enumerate at most 10240 live slots once, excluding pre-existing terrain.
    // Each of at most 128 payloads has a fixed 64 KiB bound.
    JPH::BodyIDVector existing;
    m_jolt_system->GetBodies(existing);
    std::array<bool, kPhysicsCapacity> terrain{};
    for (const auto& [chunk_id, collision] : m_chunk_bodies) {
        static_cast<void>(chunk_id);
        terrain[collision.body_id.GetIndex()] = true;
    }
    // Simulation collider slots are rebuilt from simulation inputs like terrain,
    // so they are allocator history but never captured owners.
    if (m_simulation_collisions) {
        const auto& colliders = *m_simulation_collisions;
        for (std::size_t i = 0; i < colliders.body_count; ++i) {
            terrain[colliders.bodies[i].GetIndex()] = true;
        }
    }
    const auto owner_count = static_cast<std::size_t>(
        std::count_if(existing.begin(), existing.end(), [&terrain](JPH::BodyID id) {
            return !terrain[id.GetIndex()];
        }));
    if (owner_count > kMaxRestoredBodies) {
        return false;
    }
    BodySetState staged;
    staged.slot_count = m_body_slots->slot_count;
    staged.free_slots.assign(m_body_slots->free_slots.begin(),
                             m_body_slots->free_slots.begin() + m_body_slots->free_count);
    staged.owners.resize(owner_count);
    // Jolt enumerates slots in index order. Explicit sorting documents the
    // ordering contract independently of its implementation.
    std::sort(existing.begin(), existing.end(), [](JPH::BodyID first, JPH::BodyID second) {
        return first.GetIndex() < second.GetIndex();
    });
    std::size_t owner = 0;
    for (const auto id : existing) {
        if (!terrain[id.GetIndex()] && !capture_body_owner(id, staged.owners[owner++])) {
            return false;
        }
    }
    output = std::move(staged);
    return true;
}

bool PhysicsSystem::restore_body_owners(const BodySetState& state, std::span<JPH::BodyID> bodies) {
    const auto& states = state.owners;
    if (!m_jolt_system || m_avatars.size() > kMaxRestoredAvatars ||
        states.size() > kMaxRestoredBodies || bodies.size() != states.size() ||
        state.slot_count > kPhysicsCapacity || state.free_slots.size() > state.slot_count) {
        return false;
    }
    // Explicit restore only: at most 128 recipes, each with two 64 KiB native
    // buffers. No storage or work is added to the simulation tick.
    std::vector<OwnerRecipe> recipes(states.size());
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto& recipe = recipes[i];
        if (!ParseOwner(states[i], recipe) ||
            (recipe.kind != kSphereOwner && recipe.kind != kFrozenBodyOwner)) {
            return false;
        }
        const auto& saved = states[i];
        if (saved.suspended) {
            const auto finite = [](const glm::vec3& value) {
                return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
            };
            if (recipe.kind != kSphereOwner || !finite(saved.retained_velocity) ||
                !finite(saved.retained_angular_velocity)) {
                return false;
            }
            bool active = false;
            recipe.owner.Read(active);
            recipe.owner.cursor = 0;
            if (active) {
                return false;
            }
        }
        const JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(),
                                     JPH::BodyID(recipe.body));
        if (lock.Succeeded()) {
            return false;
        }
        for (std::size_t j = 0; j < i; ++j) {
            if ((recipes[j].body & JPH::BodyID::cMaxBodyIndex) ==
                (recipe.body & JPH::BodyID::cMaxBodyIndex)) {
                return false;
            }
        }
        if (recipe.kind == kSphereOwner) {
            for (std::size_t j = 0; j < i; ++j) {
                if (recipes[j].kind == kSphereOwner &&
                    recipe.global_bits != recipes[j].global_bits) {
                    return false;
                }
            }
        }
    }
    // Validate topology before creating anything, including references to bodies
    // in this batch. Missing external bodies are a refusal, never a dropped contact.
    for (const auto& recipe : recipes) {
        if (recipe.kind != kSphereOwner) {
            continue;
        }
        for (std::size_t ref = 0; ref < recipe.reference_count; ++ref) {
            const auto id = recipe.references[ref];
            const bool staged =
                std::any_of(recipes.begin(), recipes.end(), [id](const auto& candidate) {
                    return candidate.body == id;
                });
            if (!staged) {
                const JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(),
                                             JPH::BodyID(id));
                if (!lock.Succeeded()) {
                    return false;
                }
            }
        }
    }
    JPH::BodyIDVector existing;
    m_jolt_system->GetBodies(existing);
    for (const auto id : existing) {
        const JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(), id);
        if (!lock.Succeeded() || !lock.GetBody().IsStatic()) {
            return false;
        }
        for (const auto& recipe : recipes) {
            if (id.GetIndex() == JPH::BodyID(recipe.body).GetIndex()) {
                return false;
            }
        }
    }
    if (!ValidateOwnerAllocator(state, recipes, existing, m_body_slots->slot_count, {})) {
        return false;
    }
    std::vector<OwnerRecipe::ContactKey> pairs;
    std::vector<OwnerRecipe::ContactKey> contacts;
    // Counts also bound sorting and staging; actual byte admission follows.
    for (const auto& recipe : recipes) {
        if (recipe.pair_count > kPhysicsCapacity - pairs.size() ||
            recipe.contact_count > kPhysicsCapacity - contacts.size()) {
            return false;
        }
        pairs.insert(pairs.end(), recipe.pairs.begin(), recipe.pairs.begin() + recipe.pair_count);
        contacts.insert(contacts.end(),
                        recipe.contacts.begin(),
                        recipe.contacts.begin() + recipe.contact_count);
    }
    try {
        CheckUniqueKeys(pairs);
        CheckUniqueKeys(contacts);
    } catch (const OwnerStateRefusal&) {
        return false;
    }
    if (!ValidateOwnerContactCache(recipes, m_jolt_system->GetPhysicsSettings())) {
        return false;
    }
    const bool has_capsules = std::any_of(recipes.begin(), recipes.end(), [](const auto& recipe) {
        return recipe.kind == kFrozenBodyOwner;
    });
    JPH::Ref<JPH::Shape> avatar_shape = m_avatar_shape;
    if (has_capsules && avatar_shape == nullptr) {
        avatar_shape = FeetCapsule(0.9f, 1.3f);
    }
    std::vector<UnassignedOwnerBody> staged;
    staged.reserve(recipes.size());
    std::size_t sphere_count = 0;
    for (auto& recipe : recipes) {
        JPH::BodyCreationSettings settings;
        if (recipe.kind == kSphereOwner) {
            settings = JPH::BodyCreationSettings(new JPH::SphereShape(recipe.radius),
                                                 JPH::RVec3::sZero(),
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Dynamic,
                                                 Layers::MOVING);
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            ++sphere_count;
        } else {
            settings = JPH::BodyCreationSettings(avatar_shape,
                                                 recipe.feet,
                                                 JPH::Quat::sIdentity(),
                                                 JPH::EMotionType::Static,
                                                 Layers::NON_MOVING);
        }
        UnassignedOwnerBody body(m_body_interface->CreateBodyWithoutID(settings),
                                 UnassignedOwnerBodyDeleter{m_body_interface});
        if (!body) {
            return false;
        }
        if (recipe.kind == kSphereOwner) {
            // Stage the saved bounds before broadphase insertion, including
            // sleeping bodies that will not integrate on the following tick.
            bool active = false;
            recipe.owner.Read(active);
            body->RestoreState(recipe.owner);
            recipe.owner.cursor = 0;
        }
        staged.push_back(std::move(body));
    }
    auto scratch = PrepareOwnerAllocatorScratch(
        *m_body_interface, !state.free_slots.empty(), Layers::NON_MOVING);
    if (!state.free_slots.empty() && !scratch) {
        return false;
    }
    // Register every restored sphere, even while terrain collisions are off, so a
    // later durable-residency activation covers them. Registered spheres keep
    // their live order; unregistered ones follow in payload order. Duplicate,
    // out-of-range and non-sphere slots refuse before commit, so the zero
    // sentinel's sort key can never tie with a registered slot.
    constexpr auto kRegistrationCapacity =
        std::tuple_size_v<decltype(SimulationCollisions::dynamic_owners)>;
    std::vector<std::size_t> registration_order;
    registration_order.reserve(recipes.size());
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        const auto slot = states[i].registration_slot;
        if (slot > kRegistrationCapacity || (slot != 0 && recipes[i].kind != kSphereOwner)) {
            return false;
        }
        if (recipes[i].kind == kSphereOwner) {
            registration_order.push_back(i);
        }
    }
    const auto registration_key = [&states](std::size_t i) {
        const auto slot = states[i].registration_slot;
        return slot == 0 ? std::numeric_limits<std::uint32_t>::max() : slot;
    };
    std::stable_sort(registration_order.begin(),
                     registration_order.end(),
                     [&registration_key](std::size_t a, std::size_t b) {
                         return registration_key(a) < registration_key(b);
                     });
    for (std::size_t i = 1; i < registration_order.size(); ++i) {
        const auto slot = states[registration_order[i]].registration_slot;
        if (slot != 0 && slot == states[registration_order[i - 1]].registration_slot) {
            return false;
        }
    }
    const bool needs_registrations = simulation_collisions_enabled() || !registration_order.empty();
    std::unique_ptr<SimulationCollisions> registrations;
    if (needs_registrations && !m_simulation_collisions) {
        registrations = std::make_unique<SimulationCollisions>();
    }
    // All refusals and allocations precede commit. Empty both live cache banks
    // so replay has exactly the allocator state validated above, including when
    // the world previously contained bodies that have since been removed.
    {
        OwnerNativeStream empty;
        empty.Write(JPH::EStateRecorderState::Contacts);
        empty.Write(std::uint32_t{0});
        empty.Write(std::uint32_t{0});
        for (unsigned bank = 0; bank < 2; ++bank) {
            empty.cursor = 0;
            static_cast<void>(m_jolt_system->RestoreState(empty));
        }
    }
    for (auto& recipe : recipes) {
        if (recipe.kind != kSphereOwner) {
            continue;
        }
        --sphere_count;
        recipe.scene.SetIsLastPart(sphere_count == 0);
        if (!m_jolt_system->RestoreState(recipe.scene)) {
            throw std::logic_error("Validated owner contact cache did not fit");
        }
    }
    m_avatar_shape = avatar_shape;
    if (registrations) {
        m_simulation_collisions = std::move(registrations);
    }
    if (m_simulation_collisions) {
        // Restore replaces the complete owner set. Existing live bodies were
        // validated static above; release all previous sphere registrations.
        m_simulation_collisions->dynamic_count = 0;
        m_simulation_collisions->dynamic_slots.fill(0);
        m_simulation_collisions->ready = false;
    }
    for (std::size_t i = 0; i < recipes.size(); ++i) {
        auto* body = staged[i].get();
        AssignRestoredOwnerBody(*m_body_interface, body, JPH::BodyID(recipes[i].body));
        staged[i].release();
        m_body_interface->AddBody(body->GetID(), JPH::EActivation::DontActivate);
        if (recipes[i].kind == kSphereOwner) {
            m_jolt_system->RestoreBodyState(*body, recipes[i].owner);
        }
        bodies[i] = JPH::BodyID(recipes[i].body);
    }
    if (needs_registrations) {
        for (const auto i : registration_order) {
            const auto& saved = states[i];
            auto& registrations_state = *m_simulation_collisions;
            const auto id = bodies[i];
            registrations_state.dynamic_owners[registrations_state.dynamic_count++] = {
                .id = id,
                .velocity = JPH::Vec3(saved.retained_velocity.x,
                                      saved.retained_velocity.y,
                                      saved.retained_velocity.z),
                .angular_velocity = JPH::Vec3(saved.retained_angular_velocity.x,
                                              saved.retained_angular_velocity.y,
                                              saved.retained_angular_velocity.z),
                .suspended = saved.suspended,
                .was_active = saved.was_active};
            registrations_state.dynamic_slots[id.GetIndex()] =
                static_cast<std::uint32_t>(registrations_state.dynamic_count);
            if (saved.suspended) {
                m_body_interface->SetMotionType(
                    id, JPH::EMotionType::Kinematic, JPH::EActivation::DontActivate);
            }
        }
    }
    RestoreOwnerFreeSlots(*m_body_interface, scratch.get(), state.free_slots);
    m_body_slots->slot_count = state.slot_count;
    m_body_slots->free_count = static_cast<std::uint32_t>(state.free_slots.size());
    std::copy(state.free_slots.begin(), state.free_slots.end(), m_body_slots->free_slots.begin());
    return true;
}

bool PhysicsSystem::player_has_space_to_stand() const {
    if (!m_player_character)
        return true;

    // A small, local class that implements Jolt's abstract collector interface.
    // This is the intended use pattern for the library.
    class StandUpCollector final : public CollideShapeCollector {
    public:
        StandUpCollector() = default;

        // The virtual function we MUST implement. It's called for each hit.
        virtual void AddHit(const CollideShapeResult&) override {
            // A hit was found, so there is no space to stand.
            mHadHit = true;
            // Force the query to stop immediately. This is a critical optimization.
            ForceEarlyOut();
        }

        bool HadHit() const {
            return mHadHit;
        }

    private:
        bool mHadHit = false;
    };

    // Create an instance of our custom collector.
    StandUpCollector collector;

    // Note: We use the cached m_player_stand_shape here for consistency
    const JPH::Shape* standing_shape = m_player_stand_shape.GetPtr();

    // CollideShape expects the target shape's center-of-mass transform, including
    // the same offset and padding used by CharacterVirtual's own shape query.
    const JPH::RMat44 transform =
        JPH::RMat44::sRotationTranslation(m_player_character->GetRotation(),
                                          m_player_character->GetPosition())
            .PreTranslated(m_player_character->GetShapeOffset() + standing_shape->GetCenterOfMass())
            .PostTranslated(m_player_character->GetCharacterPadding() *
                            m_player_character->GetUp());

    CollideShapeSettings settings;
    settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;

    m_jolt_system->GetNarrowPhaseQuery().CollideShape(standing_shape,
                                                      JPH::Vec3::sReplicate(1.0f),
                                                      transform,
                                                      settings,
                                                      JPH::RVec3::sZero(),
                                                      collector,
                                                      BroadPhaseLayerFilterAll(),
                                                      ObjectLayerFilterAll(),
                                                      BodyFilter());

    // If our collector had a hit, there's no space to stand.
    return !collector.HadHit();
}

// ===  INTEGRATION ===

PhysicsSystem::AudioRaycastResult PhysicsSystem::audio_raycast(const glm::vec3& from,
                                                               const glm::vec3& to) const {
    AudioRaycastResult result;

    if (!m_jolt_system)
        return result;

    JPH::Vec3 ray_start(from.x, from.y, from.z);
    JPH::Vec3 ray_direction = JPH::Vec3(to.x, to.y, to.z) - ray_start;
    float ray_length = ray_direction.Length();

    if (ray_length < 0.001f)
        return result; // Too short

    ray_direction = ray_direction.Normalized();

    JPH::RRayCast ray(ray_start, ray_direction * ray_length);
    JPH::RayCastResult closest_hit;

    if (m_jolt_system->GetNarrowPhaseQuery().CastRay(ray,
                                                     closest_hit,
                                                     BroadPhaseLayerFilterAll(),
                                                     ObjectLayerFilterAll(),
                                                     JPH::BodyFilter())) {
        result.hit = true;
        result.distance = closest_hit.mFraction * ray_length;
        JPH::Vec3 hit_pos = ray_start + ray_direction * result.distance;
        result.hit_point = {hit_pos.GetX(), hit_pos.GetY(), hit_pos.GetZ()};

        // Query the hit body for the real surface normal and terrain shape identity.
        // Chunk terrain uses heightfields; frozen capsules share its object layer.
        bool terrain_hit = false;
        JPH::BodyLockRead lock(m_jolt_system->GetBodyLockInterface(), closest_hit.mBodyID);
        if (lock.Succeeded()) {
            const JPH::Body& body = lock.GetBody();
            JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(closest_hit.mSubShapeID2, hit_pos);
            // Opposition invariant: the normal must face AGAINST the incoming
            // ray (load-bearing for from-below casts, where the heightfield's
            // up-facing triangle normal points along the ray).
            if (normal.Dot(ray_direction) > 0.0f) {
                normal = -normal;
            }
            result.surface_normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
            terrain_hit = body.GetShape()->GetSubType() == JPH::EShapeSubType::HeightField;
        }

        if (terrain_hit && m_world_system) {
            // Terrain hit with the worldgen seam attached: biome-aware surface
            // material. For a heightfield hit the hit y IS the cached terrain
            // height, so no shaped-height recompute is needed.
            const MaterialType material = m_world_system->SurfaceVertexMaterial(
                result.hit_point.x, result.hit_point.z, result.hit_point.y);
            result.material_type = static_cast<int>(material);
            result.material_absorption = get_material_audio_absorption(result.material_type);
        } else if (m_world_system) {
            // Non-terrain body: no worldgen material applies; classify
            // as stone.
            result.material_type = static_cast<int>(MaterialType::Stone);
            result.material_absorption = get_material_audio_absorption(result.material_type);
        } else {
            // Legacy Y-band classification (no world system attached, e.g.
            // standalone tests) - byte-identical to the pre-seam behaviour.
            if (result.hit_point.y < 10.0f) {
                result.material_type = 0; // Stone
                result.material_absorption = 0.15f;
            } else if (result.hit_point.y < 50.0f) {
                result.material_type = 1; // Dirt
                result.material_absorption = 0.25f;
            } else {
                result.material_type = 2; // Grass/vegetation
                result.material_absorption = 0.4f;
            }
        }
    }

    return result;
}

float PhysicsSystem::calculate_audio_occlusion(const glm::vec3& source,
                                               const glm::vec3& listener) const {
    // Primary line-of-sight check
    AudioRaycastResult primary_ray = audio_raycast(source, listener);

    if (!primary_ray.hit) {
        return 0.0f; // Clear line of sight
    }

    float total_occlusion = 0.0f;
    float source_listener_distance = glm::distance(source, listener);

    // If we hit something, calculate occlusion based on material and geometry
    float obstruction_factor = primary_ray.distance / source_listener_distance;
    total_occlusion += primary_ray.material_absorption * obstruction_factor;

    // Additional rays for more accurate occlusion (performance vs accuracy trade-off)
    const std::vector<glm::vec3> offsets = {
        {0.3f, 0.0f, 0.0f},
        {-0.3f, 0.0f, 0.0f}, // Left/right
        {0.0f, 0.3f, 0.0f},
        {0.0f, -0.3f, 0.0f}, // Up/down
    };

    int clear_paths = 0;
    for (const auto& offset : offsets) {
        AudioRaycastResult ray = audio_raycast(source + offset, listener + offset);
        if (!ray.hit) {
            clear_paths++;
        } else {
            float offset_obstruction =
                ray.distance / glm::distance(source + offset, listener + offset);
            total_occlusion +=
                ray.material_absorption * offset_obstruction * 0.2f; // Reduced weight
        }
    }

    // Reduce occlusion if we have alternative paths
    float path_factor =
        1.0f - (static_cast<float>(clear_paths) / static_cast<float>(offsets.size()) * 0.6f);
    total_occlusion *= path_factor;

    return std::clamp(total_occlusion, 0.0f, 0.95f); // Max 95% occlusion
}

std::vector<glm::vec3> PhysicsSystem::calculate_audio_reflection_points(const glm::vec3& source,
                                                                        const glm::vec3& listener,
                                                                        int max_bounces) const {
    std::vector<glm::vec3> reflection_points;

    if (!m_jolt_system || max_bounces <= 0)
        return reflection_points;

    glm::vec3 current_pos = source;
    glm::vec3 target = listener;

    for (int bounce = 0; bounce < max_bounces; ++bounce) {
        AudioRaycastResult ray = audio_raycast(current_pos, target);

        if (!ray.hit) {
            break; // Direct path found, no more reflections
        }

        // Calculate reflection point
        glm::vec3 incident = glm::normalize(ray.hit_point - current_pos);
        glm::vec3 reflected =
            incident - 2.0f * glm::dot(incident, ray.surface_normal) * ray.surface_normal;

        reflection_points.push_back(ray.hit_point);

        // Set up for next bounce
        current_pos =
            ray.hit_point + ray.surface_normal * 0.01f; // Small offset to avoid self-intersection

        target = current_pos + reflected * 10.0f; // Extend reflection ray

        // Early termination if reflection quality becomes too poor
        if (ray.material_absorption > 0.8f)
            break;
    }

    return reflection_points;
}

float PhysicsSystem::get_material_audio_absorption(int material_type) const {
    // Keyed by static_cast<int>(MaterialType) codes (core/Types.h).
    switch (material_type) {
        case static_cast<int>(MaterialType::Stone):
            return 0.15f; // Stone - hard, reflective
        case static_cast<int>(MaterialType::Soil):
            return 0.25f; // Soil - moderate absorption
        case static_cast<int>(MaterialType::Grass):
            return 0.4f; // Grass - soft, absorbing
        case static_cast<int>(MaterialType::Sand):
            return 0.6f; // Sand - high absorption
        case static_cast<int>(MaterialType::Deepslate):
            return 0.1f; // Deepslate - very hard, highly reflective
        case static_cast<int>(MaterialType::LuminCrystal):
            return 0.05f; // LuminCrystal - crystalline, highly reflective
        case static_cast<int>(MaterialType::Water):
            return 0.9f; // Water - high absorption for airborne sound
        default:
            return 0.2f; // Default medium absorption
    }
}

// === BATCHED PHYSICS QUERY SYSTEM ===

int PhysicsSystem::BatchedPhysicsQueries::QueueRaycast(
    const glm::vec3& from,
    const glm::vec3& to,
    std::function<void(const AudioRaycastResult&)> callback,
    float priority) {
    int query_id = m_next_query_id++;
    m_queued_queries.push_back({from, to, std::move(callback), query_id, priority});
    return query_id;
}

void PhysicsSystem::BatchedPhysicsQueries::ProcessBatch(const PhysicsSystem* physics_system) {
    if (m_queued_queries.empty()) {
        m_processed_this_frame = 0;
        return;
    }

    // Sort queries by priority (higher priority first) and then spatially
    std::sort(m_queued_queries.begin(),
              m_queued_queries.end(),
              [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
                  return a.priority > b.priority;
              });

    // Limit processing to avoid frame spikes
    const std::size_t queries_to_process =
        std::min(m_queued_queries.size(), m_max_queries_per_frame);

    // Process highest priority queries first
    std::vector<BatchedRaycastQuery> high_priority_queries;
    std::vector<BatchedRaycastQuery> remaining_queries;

    for (std::size_t i = 0; i < queries_to_process; ++i) {
        high_priority_queries.push_back(m_queued_queries[i]);
    }

    for (std::size_t i = queries_to_process; i < m_queued_queries.size(); ++i) {
        remaining_queries.push_back(m_queued_queries[i]);
    }

    // Sort the queries we're processing spatially for better cache performance
    SortQueriesSpatially(high_priority_queries);

    m_results_buffer.clear();
    m_results_buffer.reserve(high_priority_queries.size());

    // Batch process raycasts
    for (const auto& query : high_priority_queries) {
        AudioRaycastResult result = physics_system->audio_raycast(query.from, query.to);
        m_results_buffer.push_back({result, query.query_id});

        // Immediately invoke callback for this result
        if (query.callback) {
            query.callback(result);
        }
    }

    m_processed_this_frame = high_priority_queries.size();

    // Replace queued queries with remaining ones
    m_queued_queries = std::move(remaining_queries);
}

void PhysicsSystem::BatchedPhysicsQueries::ClearCompleted() {
    m_results_buffer.clear();
    m_processed_this_frame = 0;
    // Note: We don't clear m_queued_queries here as they represent pending work
}

void PhysicsSystem::BatchedPhysicsQueries::SortQueriesSpatially(
    std::vector<BatchedRaycastQuery>& queries) {
    // Simple spatial sorting based on query start position
    // This improves cache coherency when accessing the physics world
    std::sort(queries.begin(),
              queries.end(),
              [](const BatchedRaycastQuery& a, const BatchedRaycastQuery& b) {
                  // Morton encoding for better spatial locality
                  auto morton_encode = [](float x, float y, float z) -> uint64_t {
                      // Simple 3D morton encoding (interleave bits)
                      uint32_t ix = static_cast<uint32_t>(x * 100.0f) & 0x3FF; // 10 bits
                      uint32_t iy = static_cast<uint32_t>(y * 100.0f) & 0x3FF;
                      uint32_t iz = static_cast<uint32_t>(z * 100.0f) & 0x3FF;

                      uint64_t result = 0;
                      for (int i = 0; i < 10; ++i) {
                          result |= ((ix & (1u << i)) << (2 * i)) |
                                    ((iy & (1u << i)) << (2 * i + 1)) |
                                    ((iz & (1u << i)) << (2 * i + 2));
                      }
                      return result;
                  };

                  uint64_t morton_a = morton_encode(a.from.x, a.from.y, a.from.z);
                  uint64_t morton_b = morton_encode(b.from.x, b.from.y, b.from.z);

                  return morton_a < morton_b;
              });
}

} // namespace Systems
} // namespace Luminumbra

namespace Luminumbra::world {

bool GameSession::CapturePlayerOwner(World::PhysicsOwnerState& output) const {
    return m_physicsSystem &&
           Systems::PhysicsSystem::OwnerContinuation(*m_physicsSystem).capture_player_owner(output);
}

bool GameSession::RestorePlayerOwner(const World::PhysicsOwnerState& state) {
    return m_physicsSystem &&
           Systems::PhysicsSystem::OwnerContinuation(*m_physicsSystem).restore_player_owner(state);
}

bool GameSession::CaptureAvatarOwner(const World::PlayerAvatar& avatar,
                                     std::size_t physics_index,
                                     World::PlayerAvatarOwnerState& output) const {
    if (!m_physicsSystem) {
        return false;
    }
    World::PlayerAvatarOwnerState staged;
    staged.avatar = avatar;
    if (!Systems::PhysicsSystem::OwnerContinuation(*m_physicsSystem)
             .capture_avatar_owner(physics_index, staged.physics)) {
        return false;
    }
    if (!World::ValidateAvatarOwner(staged)) {
        return false;
    }
    output = staged;
    return true;
}

bool GameSession::RestoreAvatarOwner(const World::PlayerAvatarOwnerState& state,
                                     World::PlayerAvatar& avatar,
                                     std::size_t& physics_index,
                                     std::span<const World::PhysicsOwnerState> remaining_frozen) {
    if (!m_physicsSystem || GetSimulationTickCount() != 0 ||
        state.avatar.player_id != m_physicsSystem->avatar_character_slot_count()) {
        return false;
    }
    if (!World::ValidateAvatarOwner(state) ||
        !Systems::PhysicsSystem::OwnerContinuation(*m_physicsSystem)
             .restore_avatar_owner(state.physics, physics_index, remaining_frozen)) {
        return false;
    }
    avatar = state.avatar;
    return true;
}

} // namespace Luminumbra::world
