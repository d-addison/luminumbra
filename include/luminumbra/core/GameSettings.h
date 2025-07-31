#pragma once

namespace Luminumbra::Core {

struct GameSettings {
    bool vsync = true;
    int shadowQuality = 2;      // 0:Low, 1:Medium, 2:High, 3:Ultra
    int textureFiltering = 1;   // 0:Bilinear, 1:Trilinear
    float masterVolume = 0.6f;
    float musicVolume = 0.3f;
    float effectsVolume = 0.5f;
    float mouseSensitivity = 0.5f;
    bool invertY = false;
};

} // namespace Luminumbra::Core