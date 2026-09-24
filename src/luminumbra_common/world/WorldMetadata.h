#pragma once

#include "luminumbra/core/Types.h"

#include <ctime>
#include <string>

namespace Luminumbra::world {

struct WorldMetadata {
    std::string name;
    std::string seed;
    std::string worldType;
    std::string worldId;
    std::time_t creationTime = 0;
    Vec3 spawnPoint{};
};

} // namespace Luminumbra::world
