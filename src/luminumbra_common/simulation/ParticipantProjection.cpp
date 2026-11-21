#include "ParticipantProjection.h"

#include "SimulationIdentity.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Luminumbra::simulation {
namespace {
namespace C = ::Luminumbra::Components;
namespace P = projection;

// A byte-stream FNV-1a fold, identical to StableChecksum without allocating a
// canonical byte buffer proportional to the participant state.
class CanonicalFold {
public:
    explicit CanonicalFold(const entt::registry& registry)
        : m_registry(registry) {}

    void Byte(std::uint8_t byte) {
        m_hash ^= byte;
        m_hash *= 1099511628211ull;
    }

    template<typename T>
    void Integer(T number) {
        using U = std::make_unsigned_t<T>;
        const auto bits = static_cast<U>(number);
        for (std::size_t byte = 0; byte < sizeof(T); ++byte)
            Byte(static_cast<std::uint8_t>((bits >> (byte * 8)) & U{255}));
    }

    void Text(std::string_view value) {
        ValidateUtf8(value);
        Integer(static_cast<std::uint64_t>(value.size()));
        for (const char byte : value)
            Byte(static_cast<std::uint8_t>(static_cast<unsigned char>(byte)));
    }

    template<typename T>
    void Value(const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            Byte(value ? 1 : 0);
        } else if constexpr (std::is_floating_point_v<T>) {
            static_assert(std::numeric_limits<T>::is_iec559);
            if (!std::isfinite(value))
                throw std::domain_error("Non-finite simulation projection field");
            // Simulation comparisons treat signed zeros as equal.
            const T canonical = value == T{0} ? T{0} : value;
            if constexpr (sizeof(T) == 4)
                Integer(std::bit_cast<std::uint32_t>(canonical));
            else {
                static_assert(sizeof(T) == 8);
                Integer(std::bit_cast<std::uint64_t>(canonical));
            }
        } else if constexpr (std::is_same_v<T, entt::entity>) {
            Reference(value);
        } else if constexpr (std::is_enum_v<T>) {
            Integer(static_cast<std::underlying_type_t<T>>(value));
        } else if constexpr (std::is_integral_v<T>) {
            Integer(value);
        } else if constexpr (std::is_same_v<T, std::string>) {
            Text(value);
        } else if constexpr (std::is_same_v<T, Vec2>) {
            Value(value.x);
            Value(value.y);
        } else if constexpr (std::is_same_v<T, Vec3>) {
            Value(value.x);
            Value(value.y);
            Value(value.z);
        } else if constexpr (requires {
                                 value.size();
                                 value.begin();
                                 value.end();
                             }) {
            Integer(static_cast<std::uint64_t>(value.size()));
            for (const auto& item : value)
                Value(item);
        } else {
            if constexpr (std::is_same_v<T, C::PlantGenomeComponent>) {
                for (const float gene : value.genes) {
                    if (!(gene >= 0.0f && gene <= 1.0f))
                        throw std::domain_error("Invalid plant genome projection field");
                }
            }
            P::Fields<T>::Visit(value, [this](std::string_view name, const auto& field) {
                ValidateDomain(name, field);
                this->Value(field);
            });
        }
    }

    template<typename T>
    void Component(const entt::registry& registry, entt::entity entity) {
        Text(P::Fields<T>::name);
        Integer(P::Fields<T>::version);
        if constexpr (std::is_empty_v<T>) {
            Value(registry.all_of<T>(entity));
        } else {
            const auto* component = registry.try_get<const T>(entity);
            Value(component != nullptr);
            if (component)
                Value(*component);
        }
    }

    [[nodiscard]] std::string Finish() const {
        constexpr std::string_view digits = "0123456789abcdef";
        std::string result(16, '0');
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = digits[(m_hash >> ((15 - index) * 4)) & 15u];
        return result;
    }

private:
    template<typename T>
    static void ValidateDomain(std::string_view name, const T& value) {
        if constexpr (std::is_floating_point_v<T>) {
            constexpr std::array unit_fields{"hunger",
                                             "stamina",
                                             "energy",
                                             "vigilance",
                                             "hunger_threshold",
                                             "thirst",
                                             "water_proximity",
                                             "activity",
                                             "level",
                                             "biomass",
                                             "pressure",
                                             "meter",
                                             "noise_pitch",
                                             "suspicious_at",
                                             "alert_at",
                                             "engaged_at",
                                             "need_pressure"};
            for (const std::string_view field : unit_fields) {
                if (name == field && !(value >= T{0} && value <= T{1}))
                    throw std::domain_error("Simulation projection field outside unit interval");
            }
            if (name == "vision_cos_half_fov" && !(value >= T{-1} && value <= T{1}))
                throw std::domain_error("Simulation projection vision cosine outside domain");
        } else if constexpr (std::is_same_v<T, ::luminumbra::ai::AwarenessState>) {
            if (value < ::luminumbra::ai::AwarenessState::Unaware ||
                value > ::luminumbra::ai::AwarenessState::Engaged)
                throw std::domain_error("Invalid awareness projection state");
        } else if constexpr (std::is_same_v<T, ::luminumbra::ai::StimulusChannel>) {
            if (static_cast<unsigned>(value) >= ::luminumbra::ai::kStimulusChannelCount)
                throw std::domain_error("Invalid stimulus projection channel");
        }
    }

    static void ValidateUtf8(std::string_view value) {
        // At most value.size() bytes, no decoded copy or locale dependency.
        for (std::size_t index = 0; index < value.size();) {
            const auto lead = static_cast<unsigned char>(value[index++]);
            if (lead < 0x80u)
                continue;
            unsigned count = 0;
            std::uint32_t scalar = 0;
            std::uint32_t minimum = 0;
            if (lead >= 0xc2u && lead <= 0xdfu) {
                count = 1;
                scalar = lead & 0x1fu;
                minimum = 0x80u;
            } else if (lead >= 0xe0u && lead <= 0xefu) {
                count = 2;
                scalar = lead & 0x0fu;
                minimum = 0x800u;
            } else if (lead >= 0xf0u && lead <= 0xf4u) {
                count = 3;
                scalar = lead & 0x07u;
                minimum = 0x10000u;
            } else {
                throw std::domain_error("Invalid UTF-8 simulation projection field");
            }
            if (count > value.size() - index)
                throw std::domain_error("Partial UTF-8 simulation projection field");
            for (unsigned offset = 0; offset < count; ++offset) {
                const auto next = static_cast<unsigned char>(value[index++]);
                if ((next & 0xc0u) != 0x80u)
                    throw std::domain_error("Invalid UTF-8 continuation in simulation projection");
                scalar = (scalar << 6) | (next & 0x3fu);
            }
            if (scalar < minimum || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu))
                throw std::domain_error("Invalid UTF-8 scalar in simulation projection");
        }
    }

    void Reference(entt::entity entity) {
        if (entity == entt::null) {
            Byte(0);
        } else if (!m_registry.valid(entity)) {
            Byte(1);
        } else {
            const auto* identity = m_registry.try_get<const C::SimulationIdentityComponent>(entity);
            if (!identity || !IsValidSimulationId(identity->id))
                throw std::domain_error("Missing simulation reference identity");
            Byte(2);
            Integer(identity->id);
        }
    }

    const entt::registry& m_registry;
    std::uint64_t m_hash = 14695981039346656037ull;
};

bool EcologyParticipant(const entt::registry& registry, entt::entity entity) {
    // Plants have their own projection. Multiple simulation roles still share
    // one identity; a plant with a creature/agent role also contributes ecology.
    return !registry.all_of<C::PlantTag>(entity) || registry.any_of<C::CreatureComponent,
                                                                    C::InstinctAgentComponent,
                                                                    C::InstinctAgent,
                                                                    C::NeedsComponent,
                                                                    C::ActionPlanComponent,
                                                                    C::ForagerComponent>(entity);
}

void AddParticipant(const entt::registry& registry,
                    entt::entity entity,
                    ParticipantProjectionScratch& scratch) {
    const auto* identity = registry.try_get<const C::SimulationIdentityComponent>(entity);
    if (!identity || !IsValidSimulationId(identity->id))
        throw std::domain_error("Missing or invalid simulation participant identity");
    if (scratch.entries.size() == scratch.entries.max_size())
        throw std::length_error("Simulation projection scratch capacity exceeded");
    scratch.entries.push_back({.id = identity->id, .entity = entity});
}

void OrderParticipants(ParticipantProjectionScratch& scratch) {
    auto& entries = scratch.entries;
    auto& radix = scratch.radix;
    if (entries.size() > radix.max_size())
        throw std::length_error("Simulation projection radix capacity exceeded");
    radix.resize(entries.size());
    // Eight fixed 8-bit passes. Counts and prefix sums cannot exceed entries.size().
    for (unsigned shift = 0; shift < 64; shift += 8) {
        std::array<std::size_t, 256> counts{};
        for (const auto& entry : entries)
            ++counts[(entry.id >> shift) & 255u];
        std::size_t prefix = 0;
        for (auto& count : counts) {
            const auto size = count;
            count = prefix;
            prefix += size;
        }
        for (const auto& entry : entries)
            radix[counts[(entry.id >> shift) & 255u]++] = entry;
        entries.swap(radix);
    }
    // Each admitted entity was gathered once. Equal IDs are an admission
    // invariant violation, never a runtime-handle tie-break.
    for (std::size_t index = 1; index < entries.size(); ++index) {
        if (entries[index - 1].id == entries[index].id)
            throw std::domain_error("Duplicate simulation participant identity");
    }
}

std::string Project(const entt::registry& registry,
                    bool plants,
                    std::uint64_t next_id,
                    ParticipantProjectionScratch& scratch) {
    static_assert(sizeof(int) == 4 && sizeof(std::size_t) <= sizeof(std::uint64_t));
    scratch.entries.clear();
    scratch.participant_visits = 0;
    scratch.source_visits = 0;
    if (plants) {
        if (next_id == 0 || next_id > world::ActiveRegionLedger::kPersistentIdLimit)
            throw std::domain_error("Invalid simulation allocator cursor");
        for (const auto entity : registry.view<const C::PlantTag>()) {
            ++scratch.source_visits;
            AddParticipant(registry, entity, scratch);
        }
    } else {
        // Identity is also admission for authoritative reference-only targets
        // (for example a location with only a transform). Render dressing never
        // acquires an identity. Gather once; role pools below only audit admission.
        for (const auto entity : registry.view<const C::SimulationIdentityComponent>()) {
            ++scratch.source_visits;
            if (EcologyParticipant(registry, entity))
                AddParticipant(registry, entity, scratch);
        }
        P::ParticipantComponents::Visit([&]<typename T>() {
            // A transform alone is presentation, and consumes neither identity
            // nor projection work. All opt-in simulation pools are inventoried.
            if constexpr (!std::is_same_v<T, C::TransformComponent> &&
                          !std::is_same_v<T, C::PendingDeletion>) {
                for (const auto entity : registry.view<const T>()) {
                    ++scratch.source_visits;
                    if (EcologyParticipant(registry, entity) &&
                        !registry.all_of<C::SimulationIdentityComponent>(entity))
                        throw std::domain_error("Missing simulation participant identity");
                }
            }
        });
    }
    if (scratch.entries.empty() && (!plants || next_id == 1))
        return {};
    OrderParticipants(scratch);
    CanonicalFold fold(registry);
    fold.Text(plants ? "plant:v2" : "ecology:v3");
    if (plants)
        fold.Value(next_id);
    fold.Value(static_cast<std::uint64_t>(scratch.entries.size()));
    for (const auto& entry : scratch.entries) {
        if (plants && entry.id >= next_id)
            throw std::domain_error("Simulation identity is outside the allocated range");
        fold.Value(entry.id);
        ++scratch.participant_visits;
        const auto component = [&]<typename T>() {
            fold.Component<T>(registry, entry.entity);
        };
        if (plants)
            P::PlantComponents::Visit(component);
        else
            P::ParticipantComponents::Visit(component);
    }
    return fold.Finish();
}
} // namespace

std::string ComputePlantProjection(const entt::registry& registry,
                                   std::uint64_t next_id,
                                   ParticipantProjectionScratch& scratch) {
    return Project(registry, true, next_id, scratch);
}

std::string ComputeEcologyProjection(const entt::registry& registry,
                                     ParticipantProjectionScratch& scratch) {
    return Project(registry, false, 1, scratch);
}

std::string ComputeCandidatePlantSubHash(const entt::registry& registry, std::uint64_t next_id) {
    thread_local ParticipantProjectionScratch scratch;
    return ComputePlantProjection(registry, next_id, scratch);
}

std::string ComputeCandidateEcologySubHash(const entt::registry& registry) {
    thread_local ParticipantProjectionScratch scratch;
    return ComputeEcologyProjection(registry, scratch);
}

} // namespace Luminumbra::simulation
