#pragma once

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Luminumbra::Ecs {

struct EntityComponentSnapshot {
    std::string type;
    nlohmann::json data;
};

struct EntitySnapshotRecord {
    std::uint64_t entity_id = 0;
    std::string name;
    std::vector<EntityComponentSnapshot> components;
    // Decode-only presence metadata: legacy records without an ID use fresh entities.
    // Kept out of the wire schema; ID zero remains a valid saved identity.
    bool has_entity_id = true;
};

struct EntityRegistrySnapshot {
    std::vector<EntitySnapshotRecord> entities;
};

inline const char* EntitySnapshotSchema() {
    return "luminumbra.ecs.entity_snapshot.v1";
}

inline const char* EntitySnapshotOrderContract() {
    return "entity_id_ascending_component_type_ascending";
}

inline void SortEntityRegistrySnapshot(EntityRegistrySnapshot& snapshot) {
    std::stable_sort(
        snapshot.entities.begin(), snapshot.entities.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.entity_id < rhs.entity_id;
        });

    for (EntitySnapshotRecord& entity : snapshot.entities) {
        std::sort(entity.components.begin(),
                  entity.components.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.type < rhs.type; });
    }
}

// BuildEntitySnapshotFixture (game-flavored fixture entities)
// relocated to test-support code (test/support/EntitySnapshotFixture.h).
// This engine header keeps only the schema/ordering contract and the
// serialize/load APIs; gate builders that hash a fixture now receive it as a
// parameter.

inline std::string StableEntitySnapshotDump(const nlohmann::json& value) {
    std::ostringstream stream;
    stream << std::setw(4) << value << '\n';
    return stream.str();
}

inline std::string SerializeEntityRegistrySnapshotJson(EntityRegistrySnapshot snapshot) {
    SortEntityRegistrySnapshot(snapshot);

    nlohmann::json entities = nlohmann::json::array();
    std::size_t component_count = 0;
    for (const EntitySnapshotRecord& entity : snapshot.entities) {
        nlohmann::json components = nlohmann::json::array();
        for (const EntityComponentSnapshot& component : entity.components) {
            components.push_back({{"type", component.type}, {"data", component.data}});
            ++component_count;
        }

        nlohmann::json record = {{"name", entity.name}, {"components", std::move(components)}};
        if (entity.has_entity_id)
            record["entity_id"] = entity.entity_id;
        entities.push_back(std::move(record));
    }

    nlohmann::json output = {{"schema", EntitySnapshotSchema()},
                             {"order_contract", EntitySnapshotOrderContract()},
                             {"entity_count", snapshot.entities.size()},
                             {"component_count", component_count},
                             {"entities", std::move(entities)}};
    return StableEntitySnapshotDump(output);
}

namespace snapshot_detail {
// JSON get<unsigned>() also accepts floats and narrows signed values. Admit the
// integer representation and destination range before any such conversion.
template<typename T>
inline bool ReadUnsignedInteger(const nlohmann::json& value, T& result) {
    static_assert(std::is_unsigned_v<T> && sizeof(T) <= sizeof(std::uint64_t));
    std::uint64_t number = 0;
    if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_number = value.get<std::int64_t>();
        if (signed_number < 0)
            return false;
        number = static_cast<std::uint64_t>(signed_number);
    } else {
        return false;
    }
    if (number > std::numeric_limits<T>::max())
        return false;
    result = static_cast<T>(number);
    return true;
}
} // namespace snapshot_detail

inline bool LoadEntityRegistrySnapshotJson(const std::string& json_text,
                                           EntityRegistrySnapshot& out_snapshot,
                                           std::vector<std::string>& errors) {
    try {
        const nlohmann::json snapshot = nlohmann::json::parse(json_text);
        if (snapshot.at("schema").get<std::string>() != EntitySnapshotSchema()) {
            errors.push_back("unexpected entity snapshot schema");
            return false;
        }
        if (snapshot.at("order_contract").get<std::string>() != EntitySnapshotOrderContract()) {
            errors.push_back("unexpected entity snapshot ordering contract");
            return false;
        }

        std::size_t declared_entities = 0;
        std::size_t declared_components = 0;
        if (!snapshot_detail::ReadUnsignedInteger(snapshot.at("entity_count"), declared_entities) ||
            !snapshot_detail::ReadUnsignedInteger(snapshot.at("component_count"),
                                                  declared_components)) {
            errors.push_back("invalid entity snapshot counts");
            return false;
        }
        const auto& entities = snapshot.at("entities");
        if (!entities.is_array() || entities.size() != declared_entities) {
            errors.push_back("loaded entity count does not match entity_count");
            return false;
        }
        EntityRegistrySnapshot loaded;
        std::size_t component_count = 0;
        for (const nlohmann::json& entity_json : entities) {
            EntitySnapshotRecord entity;
            entity.has_entity_id = entity_json.contains("entity_id");
            if (entity.has_entity_id && !snapshot_detail::ReadUnsignedInteger(
                                            entity_json.at("entity_id"), entity.entity_id)) {
                errors.push_back("invalid entity snapshot entity_id");
                return false;
            }
            entity.name = entity_json.at("name").get<std::string>();

            const auto& components = entity_json.at("components");
            // The actual arrays bound decoding work; never allocate from hostile counts.
            if (!components.is_array() ||
                components.size() > declared_components - component_count) {
                errors.push_back("loaded component count does not match component_count");
                return false;
            }
            for (const nlohmann::json& component_json : components) {
                entity.components.push_back(EntityComponentSnapshot{
                    component_json.at("type").get<std::string>(), component_json.at("data")});
                ++component_count;
            }
            loaded.entities.push_back(std::move(entity));
        }

        if (component_count != declared_components) {
            errors.push_back("loaded component count does not match component_count");
            return false;
        }

        SortEntityRegistrySnapshot(loaded);
        out_snapshot = std::move(loaded);
        return true;
    } catch (const std::exception& e) {
        errors.push_back(std::string("failed to load entity snapshot: ") + e.what());
        return false;
    }
}

} // namespace Luminumbra::Ecs
