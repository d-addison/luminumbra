#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include <glad/glad.h>
#include <memory>
#include <string>
#include <vector>

// Forward-declare so we don't need to include ImGui here
struct ImGuiContext;

namespace Luminumbra::Systems {
class SHIELD_WorldSystem;
}

namespace Luminumbra::Client {

class WorldGenViewer {
public:
    WorldGenViewer();
    ~WorldGenViewer();

    static const char* LayerArtifactSchema();

    //  enable the CONSTRAINED layer-graph authoring panel
    // (flagged via --worldgen-graph). Off by default so the normal inspector is
    // unchanged; when on, an "Layer Graph (constrained)" panel renders the fixed
    // pipeline stages as ImGui cards over the viewer's current params.
    void SetGraphEnabled(bool enabled) {
        m_graphEnabled = enabled;
    }
    bool GraphEnabled() const {
        return m_graphEnabled;
    }

    // The main function to call each frame. It draws the ImGui window and handles updates.
    void UpdateAndRender(bool& is_open, Systems::SHIELD_WorldSystem* main_world_system);

private:
    // Regenerates the preview texture based on current parameters.
    void RegenerateTexture();

    //  renders the constrained fixed-topology layer graph as a
    // column of ImGui stage cards (edges implicit/fixed in v1) over a preset JSON
    // derived from the viewer's current params. Read-only authoring view.
    void RenderLayerGraphPanel();

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

    //  constrained layer-graph authoring panel toggle (set from
    // the --worldgen-graph flag). Default off — the inspector is byte-identical.
    bool m_graphEnabled = false;

    // Viewer settings
    float m_sliceY = 64.0f;
    float m_zoom = 1.0f;
    float m_offsetX = 0.0f;
    float m_offsetZ = 0.0f;
    bool m_showBaseTerrainLayer = true;
    bool m_showIslandMaskLayer = true;
    bool m_showCaveLayer = true;
    bool m_showWaterLayer = true;
    bool m_showTopologyDeltas = true;

    // OpenGL texture for display
    GLuint m_textureID = 0;
    const int m_textureSize = 512;
    std::vector<unsigned char> m_pixelBuffer;
};

} // namespace Luminumbra::Client
