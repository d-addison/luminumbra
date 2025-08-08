#pragma once
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include <glad/glad.h>
#include <vector>

namespace Luminumbra::Client {

class WorldGenViewer {
public:
    WorldGenViewer();
    ~WorldGenViewer();

    void UpdateAndRender();

private:
    void RegenerateTexture();

    // A separate world system instance for live editing
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_worldSystem;
    
    // Generation parameters that can be modified in the UI
    Systems::TerrainGenParams m_params;
    int m_seed = 1337;
    bool m_paramsChanged = true; // "Dirty" flag

    // Viewer settings
    float m_sliceY = 32.0f;
    float m_zoom = 1.0f;

    // OpenGL texture for display
    GLuint m_textureID = 0;
    const int m_textureSize = 512;
    std::vector<unsigned char> m_pixelBuffer;
};

} // namespace Luminumbra::Client