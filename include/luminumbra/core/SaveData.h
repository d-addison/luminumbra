#pragma once

#include "json.hpp"
#include <glm/glm.hpp>
#include <string>

namespace glm {
    inline void to_json(nlohmann::json& j, const glm::vec3& v) {
        j = { {"x", v.x}, {"y", v.y}, {"z", v.z} };
    }
    inline void from_json(const nlohmann::json& j, glm::vec3& v) {
        j.at("x").get_to(v.x);
        j.at("y").get_to(v.y);
        j.at("z").get_to(v.z);
    }
}

namespace Luminumbra::Core {
    struct PlayerSaveData {
        glm::vec3 position;
        float stamina;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(PlayerSaveData, position, stamina);
    };

    struct WorldSaveData {
        std::string slotName;
        std::string seed;
        float timeOfDay;
        NLOHMANN_DEFINE_TYPE_INTRUSIVE(WorldSaveData, slotName, seed, timeOfDay);
    };

} // namespace Luminumbra::Core