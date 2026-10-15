#pragma once

#include "ActiveRegionLedger.h"
#include "persistence/WorldSaveService.h"

#include <entt/entt.hpp>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace luminumbra::fields {
class EnergyFieldState;
}

namespace Luminumbra::world {
class FieldPageState;
class GroundObjectStore;

// Borrowed only during capture on the simulation owner thread. No pointer or reference to
// mutable authority survives in the result. Call at a completed simulation boundary.
struct AuthorityHashInputs {
    std::filesystem::path directory;
    Persistence::RegionDigestInputs chunks;
    const entt::registry& registry;
    const ActiveRegionLedger& ledger;
    const GroundObjectStore& ground;
    const FieldPageState& fields;
    const luminumbra::fields::EnergyFieldState* energy = nullptr;
    std::string clock_bytes;
    std::string identity_bytes;
    // The host supplies its existing canonical avatar snapshot; empty means no avatars.
    std::string avatar_bytes;
};

struct AuthorityHashCapture;

// Immutable canonical projections. Accessors expose no chunks, registries or runtime handles.
class AuthorityHashState {
public:
    [[nodiscard]] const std::string& chunk_hash() const {
        return m_chunkHash;
    }
    [[nodiscard]] const std::string& terrain_hash() const {
        return m_terrainHash;
    }
    [[nodiscard]] const std::string& water_hash() const {
        return m_waterHash;
    }
    [[nodiscard]] const std::string& creature_bytes() const {
        return m_creatures;
    }
    [[nodiscard]] const std::string& plant_bytes() const {
        return m_plants;
    }
    [[nodiscard]] const std::string& energy_bytes() const {
        return m_energy;
    }
    [[nodiscard]] const std::string& scent_bytes() const {
        return m_scent;
    }
    [[nodiscard]] const std::string& authority_bytes() const {
        return m_authority;
    }
    // Raw complete content, before Frozen stamp selection.
    [[nodiscard]] const std::map<RegionKey, std::uint64_t>& region_content() const {
        return m_regionContent;
    }
    [[nodiscard]] const std::map<RegionKey, std::uint64_t>& region_contributions() const {
        return m_regionContributions;
    }
    [[nodiscard]] std::string FoldEcology(const std::string& ecology) const;
    [[nodiscard]] const std::string& AmbientBytes(std::uint8_t channels, bool weather) const;

private:
    friend AuthorityHashCapture CaptureAuthorityHashState(const AuthorityHashInputs& inputs);
    AuthorityHashState() = default;
    std::string m_chunkHash, m_terrainHash, m_waterHash;
    std::string m_creatures, m_plants, m_energy, m_scent, m_authority;
    std::map<RegionKey, std::uint64_t> m_regionContent, m_regionContributions;
    std::map<std::pair<std::uint8_t, bool>, std::string> m_ambient;
};

struct AuthorityHashCapture {
    std::optional<AuthorityHashState> state;
    std::string error;
};

// Owner-scoped freeze projection. Reads only this owner's durable region and serializes only
// its entities/content, using the same regional recipe as the full capture. Throws on failure.
[[nodiscard]] std::uint64_t CaptureRegionAuthorityDigest(const AuthorityHashInputs& inputs,
                                                         RegionKey key);

// Errors never carry a state or digest. Reads do not synchronize, admit, wake or save.
[[nodiscard]] AuthorityHashCapture CaptureAuthorityHashState(const AuthorityHashInputs& inputs);

} // namespace Luminumbra::world
