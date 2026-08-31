#pragma once

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
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
    std::sort(snapshot.entities.begin(),
              snapshot.entities.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.entity_id < rhs.entity_id; });

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

        entities.push_back({{"entity_id", entity.entity_id},
                            {"name", entity.name},
                            {"components", std::move(components)}});
    }

    nlohmann::json output = {{"schema", EntitySnapshotSchema()},
                             {"order_contract", EntitySnapshotOrderContract()},
                             {"entity_count", snapshot.entities.size()},
                             {"component_count", component_count},
                             {"entities", std::move(entities)}};
    return StableEntitySnapshotDump(output);
}

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

        EntityRegistrySnapshot loaded;
        std::size_t component_count = 0;
        for (const nlohmann::json& entity_json : snapshot.at("entities")) {
            EntitySnapshotRecord entity;
            entity.entity_id = entity_json.at("entity_id").get<std::uint64_t>();
            entity.name = entity_json.at("name").get<std::string>();

            for (const nlohmann::json& component_json : entity_json.at("components")) {
                entity.components.push_back(EntityComponentSnapshot{
                    component_json.at("type").get<std::string>(), component_json.at("data")});
                ++component_count;
            }
            loaded.entities.push_back(std::move(entity));
        }

        if (loaded.entities.size() != snapshot.at("entity_count").get<std::size_t>()) {
            errors.push_back("loaded entity count does not match entity_count");
            return false;
        }
        if (component_count != snapshot.at("component_count").get<std::size_t>()) {
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
