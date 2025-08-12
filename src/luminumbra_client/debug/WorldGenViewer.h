#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include <glad/glad.h>
#include <vector>
#include <memory>
#include <string>

// Forward-declare so we don't need to include ImGui here
struct ImGuiContext;

namespace Luminumbra::Systems { class SHIELD_WorldSystem; }

namespace Luminumbra::Client {

class WorldGenViewer {
public:
    WorldGenViewer();
    ~WorldGenViewer();

    // The main function to call each frame. It draws the ImGui window and handles updates.
    void UpdateAndRender(bool& is_open, Systems::SHIELD_WorldSystem* main_world_system);

private:
    // Regenerates the preview texture based on current parameters.
    void RegenerateTexture();

    // Re-creates the internal world system when parameters change.
    void RecreateWorldSystem();

    // A separate world system instance for live editing, doesn't affect the game world.
    std::unique_ptr<Systems::SHIELD_WorldSystem> m_viewerWorldSystem;

    // Generation parameters that can be modified in the UI.
    Systems::TerrainGenParams m_params;
    int m_seed = 1337;
    char m_seed_buf[128]; // Buffer for text input of seed

    // A "dirty" flag to trigger regeneration when a parameter is changed.
    bool m_paramsChanged = true;

    // Viewer settings
    float m_sliceY = 64.0f;
    float m_zoom = 1.0f;
    float m_offsetX = 0.0f;
    float m_offsetZ = 0.0f;

    // OpenGL texture for display
    GLuint m_textureID = 0;
    const int m_textureSize = 512;
    std::vector<unsigned char> m_pixelBuffer;
};

} // namespace Luminumbra::Client