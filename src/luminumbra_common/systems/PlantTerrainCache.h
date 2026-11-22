#pragma once

#include "luminumbra/core/Types.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace luminumbra::foliage {

// Derived owner-thread state only; never serialized or included in simulation identity.
// Slots follow admitted sampling order. Roster reordering can cause extra misses;
// exact coordinate bits and revisions are checked even when a slot changes plants.
class PlantTerrainCache {
public:
    struct Sample {
        float height = 0.0f;
        Luminumbra::MaterialType material = Luminumbra::MaterialType::Air;
        float soil = 0.5f;
        float temperature = 0.55f;
    };

    template<typename World>
    void BeginTick(const World* world) {
        m_cursor = 0;
        m_revision = world ? Revision{world,
                                      world->water_seed_terrain_generation(),
                                      world->terrain_edit_revision(),
                                      world->water_epoch()}
                           : Revision{};
    }

    template<typename World>
    Sample At(const World* world, float x, float z) {
        if (!world) {
            return {};
        }
        const Key key{std::bit_cast<std::uint32_t>(x), std::bit_cast<std::uint32_t>(z), m_revision};
        const bool retained = m_cursor < m_entries.size();
        const auto index = m_cursor++;
        if (retained && m_entries[index].key == key && Cacheable()) {
            return m_entries[index].sample;
        }
        ++m_misses;
        Sample sample;
        sample.height = world->GetTerrainHeightAt(x, z);
        sample.material = world->SurfaceVertexMaterial(x, z, sample.height);
        switch (sample.material) {
            case Luminumbra::MaterialType::Grass:
                sample.soil = 0.95f;
                break;
            case Luminumbra::MaterialType::Soil:
                sample.soil = 0.85f;
                break;
            case Luminumbra::MaterialType::Sand:
                sample.soil = 0.45f;
                break;
            case Luminumbra::MaterialType::Stone:
                sample.soil = 0.30f;
                break;
            default:
                sample.soil = 0.25f;
                break;
        }
        const float altitude = sample.height - static_cast<float>(Luminumbra::SEA_LEVEL);
        const float temperature = 0.60f - (altitude > 0.0f ? altitude : 0.0f) * 0.00045f;
        sample.temperature = temperature < 0.0f ? 0.0f : (temperature > 1.0f ? 1.0f : temperature);
        if (retained) {
            m_entries[index] = Entry{key, sample};
        } else {
            // One slot per admitted growth sample; vector::max_size bounds allocation arithmetic.
            m_entries.push_back(Entry{key, sample});
        }
        return sample;
    }

    void EndTick() {
        // Drop departed sampling slots; retain capacity up to the participating-roster high water.
        m_entries.resize(m_cursor);
    }

    [[nodiscard]] std::uint64_t misses() const {
        return m_misses;
    }
    [[nodiscard]] std::size_t size() const {
        return m_entries.size();
    }

private:
    struct Revision {
        const void* world = nullptr;
        std::uint64_t generation = 0;
        std::uint64_t authority = 0;
        std::uint64_t water = 0;
        bool operator==(const Revision&) const = default;
    };
    struct Key {
        std::uint32_t x = 0;
        std::uint32_t z = 0;
        Revision revision;
        bool operator==(const Key&) const = default;
    };
    struct Entry {
        Key key;
        Sample sample;
    };
    [[nodiscard]] bool Cacheable() const {
        constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
        return m_revision.generation != limit && m_revision.authority != limit &&
               m_revision.water != limit;
    }
    Revision m_revision;
    std::vector<Entry> m_entries;
    std::size_t m_cursor = 0;
    std::uint64_t m_misses = 0;
};

} // namespace luminumbra::foliage
