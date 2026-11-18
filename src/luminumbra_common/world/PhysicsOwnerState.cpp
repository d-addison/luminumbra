#include "PlayerAvatar.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace Luminumbra::World {
namespace {
#ifdef JPH_DOUBLE_PRECISION
constexpr std::uint32_t kOwnerVersion = 0x00010001u;
#else
constexpr std::uint32_t kOwnerVersion = 1;
#endif
constexpr std::uint32_t kAvatarOwner = 1;
constexpr std::uint32_t kPlayerOwner = 2;
constexpr std::uint32_t kSphereOwner = 3;
constexpr std::uint32_t kMaxOwnerContacts = 256;
constexpr std::uint32_t kMaxOwnerPairs = 64;
constexpr std::uint32_t kPhysicsCapacity = 10240;
struct OwnerStateRefusal {};

// Version 1 uses 32-bit words for IDs, enums, bools and float32 values, and
// float64 positions/sleep offsets. Precision is encoded in the version so a
// payload is refused before values can be narrowed by another Jolt build.
// Reads explicit little-endian words, never native Jolt storage or padding.
class OwnerReader {
public:
    explicit OwnerReader(const PhysicsOwnerState& input)
        : m_input(input) {
        require(input.size <= input.bytes.size());
    }
    static void require(bool condition) {
        if (!condition) {
            throw OwnerStateRefusal{};
        }
    }
    void require_words(std::uint32_t count) const {
        require(m_cursor <= m_input.size && count <= (m_input.size - m_cursor) / 4);
    }
    std::uint32_t word(std::uint32_t maximum = UINT32_MAX) {
        require_words(1);
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= static_cast<std::uint32_t>(m_input.bytes[m_cursor++]) << (8 * i);
        }
        require(value <= maximum);
        return value;
    }
    void finish() const {
        require(m_cursor == m_input.size);
    }
    float number() {
        const auto value = std::bit_cast<float>(word());
        require(std::isfinite(value));
        return value;
    }
    double wide_number() {
        const auto low = word();
        const auto high = word();
        const auto value = std::bit_cast<double>((static_cast<std::uint64_t>(high) << 32) | low);
        require(std::isfinite(value));
        return value;
    }
    JPH::Vec3 vector() {
        const auto x = number();
        const auto y = number();
        const auto z = number();
        return JPH::Vec3(x, y, z);
    }
    Vec3 position() {
        std::array<float, 3> components{};
        for (auto& component : components) {
            const auto low = word();
            const auto high = word();
            const auto bits = (static_cast<std::uint64_t>(high) << 32) | low;
            require(((bits >> 52) & 2047u) != 2047u);
#ifdef JPH_DOUBLE_PRECISION
            const auto value = std::bit_cast<double>(bits);
            constexpr auto max_coordinate =
                static_cast<double>((std::numeric_limits<float>::max)());
            require(value >= -max_coordinate && value <= max_coordinate);
            component = static_cast<float>(value);
#else
            component = std::bit_cast<float>(narrow_float_bits(bits));
#endif
        }
        return Vec3(components[0], components[1], components[2]);
    }
    void normal(bool allow_zero = false) {
        const auto value = vector();
        require(value.IsNormalized() || (allow_zero && value == JPH::Vec3::sZero()));
    }
    void quaternion() {
        const auto x = number();
        const auto y = number();
        const auto z = number();
        const auto w = number();
        require(JPH::Quat(x, y, z, w).IsNormalized());
    }
    static std::uint32_t checked_body_id(std::uint32_t id) {
        require(id == JPH::BodyID::cInvalidBodyID ||
                ((id & JPH::BodyID::cBroadPhaseBit) == 0 &&
                 (id & JPH::BodyID::cMaxBodyIndex) < kPhysicsCapacity));
        return id;
    }
    std::uint32_t body_id() {
        return checked_body_id(word());
    }
    void floats(std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            number();
        }
    }

private:
#ifndef JPH_DOUBLE_PRECISION
    // Integer IEEE-754 conversion keeps signed zero and subnormals exact even
    // when the simulation thread enables flush-to-zero/denormals-are-zero.
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
    const PhysicsOwnerState& m_input;
    std::size_t m_cursor = 0;
};

struct OwnerRecipe {
    std::uint32_t kind = 0;
    std::uint32_t flags = 0;
    glm::vec2 wish{0.0f};
    std::uint32_t body = JPH::BodyID::cInvalidBodyID;
    Vec3 feet{0.0f};
    JPH::Vec3 velocity = JPH::Vec3::sZero();
    using ContactKey = std::array<std::uint32_t, 4>;
    // Fixed bounds include all manifolds and CCD records within a 64 KiB owner.
    std::array<ContactKey, kMaxOwnerPairs> pairs{};
    std::array<ContactKey, 2 * kMaxOwnerContacts> contacts{};
    std::size_t pair_count = 0;
    std::size_t contact_count = 0;
    // Ground + character contacts + pair endpoints + CCD endpoints.
    std::array<std::uint32_t, 897> references{};
    std::size_t reference_count = 0;
    void reference(std::uint32_t id) {
        if (id != JPH::BodyID::cInvalidBodyID) {
            OwnerReader::require(reference_count < references.size());
            references[reference_count++] = id;
        }
    }
};

void CharacterSchema(OwnerReader& codec, OwnerRecipe& recipe) {
    codec.word(3);
    recipe.reference(codec.body_id());
    codec.word(UINT32_MAX); // ground subshape
    static_cast<void>(codec.position());
    codec.normal(true);                // no ground contact initially has a zero normal
    static_cast<void>(codec.vector()); // ground velocity
    recipe.feet = codec.position();
    codec.quaternion();
    recipe.velocity = codec.vector();             // linear velocity
    OwnerReader::require(codec.number() >= 0.0f); // previous delta time
    codec.word(1);                                // hit limit reached
    const auto count = codec.word(kMaxOwnerContacts);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto body = codec.body_id();
        OwnerReader::require(body != JPH::BodyID::cInvalidBodyID);
        recipe.reference(body);
        // No character-vs-character provider is installed by either controller path.
        const auto character = codec.word(UINT32_MAX);
        OwnerReader::require(character == UINT32_MAX);
        codec.word(UINT32_MAX); // subshape
        static_cast<void>(codec.position());
        static_cast<void>(codec.vector()); // contact velocity
        codec.normal();                    // contact normal
        codec.normal();                    // surface normal
        codec.floats(2);                   // distance, fraction
        codec.word(2);
        codec.word(1); // sensor
        codec.word(1); // collided
        codec.word(1); // discarded
        codec.word(1); // can push
    }
}

void BodySchema(OwnerReader& codec) {
    codec.word(1); // active list membership
    static_cast<void>(codec.position());
    codec.quaternion();
    for (unsigned i = 0; i < 4; ++i) {
        static_cast<void>(codec.vector());
    } // velocity, spin, force, torque
    // The canonical sleep offset is always three float64 scalars. Jolt omits it
    // in single-precision builds, which can accept only a zero offset.
    for (unsigned i = 0; i < 3; ++i) {
        const auto offset = codec.wide_number();
        constexpr auto max_offset = static_cast<double>((std::numeric_limits<float>::max)());
        OwnerReader::require(offset >= -max_offset && offset <= max_offset);
#ifndef JPH_DOUBLE_PRECISION
        OwnerReader::require(offset == 0.0);
#endif
    }
    // Sleep-test sphere centers and radii, followed by their accumulated timer.
    for (unsigned sphere = 0; sphere < 3; ++sphere) {
        codec.floats(3);
        OwnerReader::require(codec.number() >= 0.0f);
    }
    OwnerReader::require(codec.number() >= 0.0f); // sleep timer
    codec.word(1);
}

OwnerRecipe::ContactKey PairKeySchema(OwnerReader& codec, OwnerRecipe& recipe, bool subshapes) {
    const auto first = codec.body_id();
    const auto first_shape = subshapes ? codec.word(UINT32_MAX) : 0u;
    const auto second = codec.body_id();
    const auto second_shape = subshapes ? codec.word(UINT32_MAX) : 0u;
    OwnerReader::require(first < second && first != JPH::BodyID::cInvalidBodyID &&
                         second != JPH::BodyID::cInvalidBodyID);
    OwnerReader::require(first == recipe.body || second == recipe.body);
    if (!subshapes) {
        recipe.reference(first);
        recipe.reference(second);
    }
    return {first, first_shape, second, second_shape};
}

void CheckUniqueKeys(std::span<OwnerRecipe::ContactKey> keys) {
    std::sort(keys.begin(), keys.end());
    OwnerReader::require(std::adjacent_find(keys.begin(), keys.end()) == keys.end());
}

void SceneSchema(OwnerReader& codec, OwnerRecipe& recipe) {
    const auto flags = codec.word(15);
    OwnerReader::require(flags == 5u); // Global | Contacts
    const auto delta = codec.number();
    OwnerReader::require(delta >= 0.0f); // warm-start step delta
    static_cast<void>(codec.vector());
    const auto pairs = codec.word(kMaxOwnerPairs);
    std::uint32_t total_manifolds = 0;
    for (std::uint32_t i = 0; i < pairs; ++i) {
        const auto parent = PairKeySchema(codec, recipe, false);
        // Jolt emits sorted keys; refuse alternate orderings of the same cache.
        OwnerReader::require(recipe.pair_count == 0 ||
                             recipe.pairs[recipe.pair_count - 1] < parent);
        recipe.pairs[recipe.pair_count++] = parent;
        codec.floats(3); // relative position
        const auto rotation = codec.vector();
        // Jolt reconstructs w = sqrt(max(1 - |xyz|^2, 0)). Permit float32
        // rounding at half turns, but never publish a non-unit cached rotation.
        // 1e-4 allows float32 product/compression error near half turns while
        // rejecting length errors that would distort Jolt's cache dot product.
        const auto xyz_squared = rotation.LengthSq();
        const auto w = std::sqrt(std::max(1.0f - xyz_squared, 0.0f));
        OwnerReader::require(std::isfinite(xyz_squared) &&
                             std::abs(1.0f - (xyz_squared + w * w)) <= 1.0e-4f);
        const auto manifolds = codec.word(kMaxOwnerContacts);
        OwnerReader::require(manifolds <= kMaxOwnerContacts - total_manifolds);
        total_manifolds += manifolds;
        OwnerRecipe::ContactKey previous{};
        for (std::uint32_t j = 0; j < manifolds; ++j) {
            // SubShapeIDPair consists of four scalar IDs, never padding.
            const auto key = PairKeySchema(codec, recipe, true);
            OwnerReader::require(key[0] == parent[0] && key[2] == parent[2]);
            OwnerReader::require(j == 0 || previous < key);
            previous = key;
            recipe.contacts[recipe.contact_count++] = key;
            const auto points = codec.word(4);
            OwnerReader::require(points != 0);
            const auto normal_x = codec.number();
            const auto normal_y = codec.number();
            const auto normal_z = codec.number();
            OwnerReader::require(JPH::Vec3(normal_x, normal_y, normal_z).IsNormalized());
            for (JPH::uint16 point = 0; point < points; ++point) {
                codec.floats(9); // two positions, normal impulse, two friction impulses
            }
        }
    }
    const auto ccd = codec.word(kMaxOwnerContacts);
    OwnerRecipe::ContactKey previous{};
    for (std::uint32_t i = 0; i < ccd; ++i) {
        const auto key = PairKeySchema(codec, recipe, true);
        OwnerReader::require(i == 0 || previous < key);
        previous = key;
        recipe.reference(key[0]);
        recipe.reference(key[2]);
        recipe.contacts[recipe.contact_count++] = key;
    }
    CheckUniqueKeys(std::span(recipe.contacts).first(recipe.contact_count));
}

void OwnerSchema(OwnerReader& codec, OwnerRecipe& recipe) {
    OwnerReader::require(codec.word() == kOwnerVersion);
    recipe.kind = codec.word();
    OwnerReader::require(recipe.kind >= kAvatarOwner && recipe.kind <= kSphereOwner);
    if (recipe.kind == kSphereOwner) {
        recipe.body = OwnerReader::checked_body_id(codec.word());
        OwnerReader::require(recipe.body != JPH::BodyID::cInvalidBodyID);
        const auto radius = codec.number();
        OwnerReader::require(radius > 0.0f);
        // Reconstruct only radii whose default mass/inertia can form a dynamic
        // body. Finite radii alone can still underflow mass or overflow inertia.
        const auto properties = JPH::SphereShape(radius).GetMassProperties();
        const auto inertia = properties.mInertia.GetDiagonal3();
        OwnerReader::require(std::isfinite(properties.mMass) && properties.mMass > 0.0f &&
                             std::isfinite(2.5f / properties.mMass) &&
                             std::isfinite(inertia.GetX()) && std::isfinite(inertia.GetY()) &&
                             std::isfinite(inertia.GetZ()));
        BodySchema(codec);
        SceneSchema(codec, recipe);
    } else {
        recipe.flags = codec.word();
        OwnerReader::require(recipe.kind == kAvatarOwner
                                 ? recipe.flags <= 1u
                                 : (recipe.flags == 0u || recipe.flags == 2u));
        recipe.wish.x = codec.number();
        recipe.wish.y = codec.number();
        OwnerReader::require(recipe.flags != 1u || recipe.wish == glm::vec2(0.0f));
        OwnerReader::require(recipe.kind != kPlayerOwner || recipe.wish == glm::vec2(0.0f));
        recipe.body = OwnerReader::checked_body_id(codec.word());
        OwnerReader::require(recipe.kind == kAvatarOwner ||
                             recipe.body == JPH::BodyID::cInvalidBodyID);
        OwnerReader::require(recipe.flags == 1 || recipe.body == JPH::BodyID::cInvalidBodyID);
        CharacterSchema(codec, recipe);
    }
    if (recipe.body != JPH::BodyID::cInvalidBodyID) {
        const auto slot_count = codec.word();
        OwnerReader::require(slot_count <= kPhysicsCapacity &&
                             JPH::BodyID(recipe.body).GetIndex() < slot_count);
        const auto count = codec.word();
        OwnerReader::require(count < slot_count);
        codec.require_words(count);
        std::array<bool, kPhysicsCapacity> seen{};
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto free_slot = codec.body_id();
            const auto index = JPH::BodyID(free_slot).GetIndex();
            OwnerReader::require(index < slot_count &&
                                 index != JPH::BodyID(recipe.body).GetIndex() && !seen[index]);
            seen[index] = true;
        }
        for (std::size_t i = 0; i < recipe.reference_count; ++i) {
            const auto index = JPH::BodyID(recipe.references[i]).GetIndex();
            OwnerReader::require(index < slot_count && !seen[index]);
            OwnerReader::require(index != JPH::BodyID(recipe.body).GetIndex() ||
                                 recipe.references[i] == recipe.body);
        }
    }
    // Every reference to one allocator slot must carry the same sequence,
    // including controllers without an owned body/allocator footer. Sorting
    // only this bounded diagnostic scratch never changes the payload's order.
    auto references = std::span(recipe.references).first(recipe.reference_count);
    std::sort(references.begin(), references.end(), [](std::uint32_t first, std::uint32_t second) {
        return (first & JPH::BodyID::cMaxBodyIndex) < (second & JPH::BodyID::cMaxBodyIndex);
    });
    for (std::size_t i = 1; i < references.size(); ++i) {
        OwnerReader::require((references[i] & JPH::BodyID::cMaxBodyIndex) !=
                                 (references[i - 1] & JPH::BodyID::cMaxBodyIndex) ||
                             references[i] == references[i - 1]);
    }
    codec.finish();
}

bool ParseOwner(const PhysicsOwnerState& state, OwnerRecipe& recipe) {
    try {
        OwnerReader codec(state);
        OwnerSchema(codec, recipe);
        return true;
    } catch (const OwnerStateRefusal&) {
        return false;
    }
}

bool ValidateAvatarPhysics(const PhysicsOwnerState& state, const PlayerAvatar& avatar) {
    OwnerRecipe recipe;
    if (!ParseOwner(state, recipe) || recipe.kind != kAvatarOwner) {
        return false;
    }
    const std::array<float, 6> captured{recipe.feet.x,
                                        recipe.feet.y,
                                        recipe.feet.z,
                                        recipe.velocity.GetX(),
                                        recipe.velocity.GetY(),
                                        recipe.velocity.GetZ()};
    const std::array<float, 6> projected{avatar.position.x,
                                         avatar.position.y,
                                         avatar.position.z,
                                         avatar.velocity.x,
                                         avatar.velocity.y,
                                         avatar.velocity.z};
    for (std::size_t i = 0; i < captured.size(); ++i) {
        if (std::bit_cast<std::uint32_t>(captured[i]) !=
            std::bit_cast<std::uint32_t>(projected[i])) {
            return false;
        }
    }
    return true;
}

bool ValidAvatar(const PlayerAvatar& avatar) {
    return std::isfinite(avatar.position.x) && std::isfinite(avatar.position.y) &&
           std::isfinite(avatar.position.z) && std::isfinite(avatar.facing) &&
           std::isfinite(avatar.velocity.x) && std::isfinite(avatar.velocity.y) &&
           std::isfinite(avatar.velocity.z);
}

void WriteOwnerWord(std::span<std::uint8_t> output, std::size_t& cursor, std::uint32_t word) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output[cursor++] = static_cast<std::uint8_t>(word >> shift);
    }
}

std::uint32_t ReadOwnerWord(std::span<const std::uint8_t> input, std::size_t& cursor) {
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        result |= static_cast<std::uint32_t>(input[cursor++]) << shift;
    }
    return result;
}

} // namespace

bool ValidatePhysicsOwner(const PhysicsOwnerState& state) {
    OwnerRecipe recipe;
    return ParseOwner(state, recipe);
}

bool ValidateAvatarOwner(const PlayerAvatarOwnerState& state) {
    return ValidAvatar(state.avatar) && ValidateAvatarPhysics(state.physics, state.avatar);
}

bool EncodeAvatarOwner(const PlayerAvatarOwnerState& state,
                       std::span<std::uint8_t> output,
                       std::size_t& written) {
    // Header: version, player ID, position xyz, yaw, velocity xyz, physics size.
    // Validate first; neither output bytes nor written change on refusal.
    if (!ValidateAvatarOwner(state) || output.size() < 40 ||
        state.physics.size > output.size() - 40) {
        return false;
    }
    std::array<std::uint8_t, kMaxAvatarOwnerBytes> staged{};
    const std::span<std::uint8_t> destination(staged);
    std::size_t cursor = 0;
    WriteOwnerWord(destination, cursor, 1);
    WriteOwnerWord(destination, cursor, state.avatar.player_id);
    const std::array<float, 7> scalars{state.avatar.position.x,
                                       state.avatar.position.y,
                                       state.avatar.position.z,
                                       state.avatar.facing,
                                       state.avatar.velocity.x,
                                       state.avatar.velocity.y,
                                       state.avatar.velocity.z};
    for (const auto scalar : scalars) {
        WriteOwnerWord(destination, cursor, std::bit_cast<std::uint32_t>(scalar));
    }
    WriteOwnerWord(destination, cursor, state.physics.size);
    std::copy_n(state.physics.bytes.begin(), state.physics.size, destination.begin() + 40);
    const std::size_t total = 40 + state.physics.size;
    std::copy_n(staged.begin(), total, output.begin());
    written = total;
    return true;
}

bool DecodeAvatarOwner(std::span<const std::uint8_t> input, PlayerAvatarOwnerState& output) {
    if (input.size() < 40 || input.size() > kMaxAvatarOwnerBytes) {
        return false;
    }
    std::size_t cursor = 0;
    if (ReadOwnerWord(input, cursor) != 1) {
        return false;
    }
    PlayerAvatarOwnerState staged;
    staged.avatar.player_id = ReadOwnerWord(input, cursor);
    std::array<float, 7> scalars{};
    for (auto& scalar : scalars) {
        scalar = std::bit_cast<float>(ReadOwnerWord(input, cursor));
    }
    staged.avatar.position = Vec3(scalars[0], scalars[1], scalars[2]);
    staged.avatar.facing = scalars[3];
    staged.avatar.velocity = Vec3(scalars[4], scalars[5], scalars[6]);
    staged.physics.size = ReadOwnerWord(input, cursor);
    if (!ValidAvatar(staged.avatar) || staged.physics.size != input.size() - 40) {
        return false;
    }
    std::copy(input.begin() + 40, input.end(), staged.physics.bytes.begin());
    if (!ValidateAvatarOwner(staged)) {
        return false;
    }
    output = staged;
    return true;
}

} // namespace Luminumbra::World
