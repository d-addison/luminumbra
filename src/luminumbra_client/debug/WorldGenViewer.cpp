#include "debug/WorldGenViewer.h"
#include <imgui.h> // You will need to integrate ImGui into your build

namespace Luminumbra::Client {

WorldGenViewer::WorldGenViewer() {
    // Initial parameters, could be loaded from default.json
    m_params.base_frequency = 0.01f;
    m_params.base_amplitude = 50.0f;
    m_params.octaves = 4;
    // ... initialize other params ...

    // Create the world system instance for the viewer
    m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, m_params, m_seed);

    // Prepare pixel buffer and OpenGL texture
    m_pixelBuffer.resize(m_textureSize * m_textureSize * 3);
    glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_textureSize, m_textureSize, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

WorldGenViewer::~WorldGenViewer() {
    if (m_textureID) {
        glDeleteTextures(1, &m_textureID);
    }
}

void WorldGenViewer::UpdateAndRender() {
    // Re-create the world system if parameters have changed
    if (m_paramsChanged) {
        m_worldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, m_params, m_seed);
        RegenerateTexture();
        m_paramsChanged = false;
    }

    // --- ImGui Window ---
    ImGui::Begin("SHIELD Generator Inspector");

    // Display the generated texture
    ImGui::Text("SDF Slice at Y = %.1f", m_sliceY);
    ImGui::Image((void*)(intptr_t)m_textureID, ImVec2(m_textureSize, m_textureSize));

    // Controls
    if (ImGui::SliderFloat("Y Slice Level", &m_sliceY, -128.0f, 128.0f)) RegenerateTexture();
    if (ImGui::SliderFloat("Zoom", &m_zoom, 0.1f, 10.0f)) RegenerateTexture();
    if (ImGui::InputInt("World Seed", &m_seed)) m_paramsChanged = true;

    ImGui::Separator();
    ImGui::Text("Terrain Parameters");
    if (ImGui::SliderFloat("Frequency", &m_params.base_frequency, 0.001f, 0.1f, "%.4f")) m_paramsChanged = true;
    if (ImGui::SliderFloat("Amplitude", &m_params.base_amplitude, 0.0f, 200.0f)) m_paramsChanged = true;
    if (ImGui::SliderInt("Octaves", &m_params.octaves, 1, 8)) m_paramsChanged = true;
    if (ImGui::SliderFloat("Persistence", &m_params.persistence, 0.1f, 1.0f)) m_paramsChanged = true;
    if (ImGui::SliderFloat("Lacunarity", &m_params.lacunarity, 1.5f, 3.5f)) m_paramsChanged = true;
    if (ImGui::SliderFloat("Height Offset", &m_params.height_offset, -64.0f, 64.0f)) m_paramsChanged = true;

    ImGui::Separator();
    ImGui::Text("Features");
    if (ImGui::Checkbox("Enable Caves", &m_params.caves_enabled)) m_paramsChanged = true;
    if (ImGui::SliderFloat("Cave Frequency", &m_params.cave_frequency, 0.01f, 0.1f, "%.3f")) m_paramsChanged = true;

    ImGui::End();
}

void WorldGenViewer::RegenerateTexture() {
    for (int y = 0; y < m_textureSize; ++y) {
        for (int x = 0; x < m_textureSize; ++x) {
            float worldX = (x - m_textureSize / 2) * m_zoom;
            float worldZ = (y - m_textureSize / 2) * m_zoom;
            Vec3 world_pos(worldX, m_sliceY, worldZ);

            float density = m_worldSystem->get_density_at(world_pos);

            uint8_t r=0, g=0, b=0;
            if (density <= 0) { // Solid
                g = static_cast<uint8_t>(std::max(0.0, 150.0 + density * 5.0));
            } else { // Air
                b = static_cast<uint8_t>(std::min(255.0, 50.0 + density * 10.0));
            }
            if (std::abs(density) < 0.5f) { // Surface
                r = 255; g = 255; b = 255;
            }

            int index = (y * m_textureSize + x) * 3;
            m_pixelBuffer[index] = r;
            m_pixelBuffer[index+1] = g;
            m_pixelBuffer[index+2] = b;
        }
    }

    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_textureSize, m_textureSize, GL_RGB, GL_UNSIGNED_BYTE, m_pixelBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace Luminumbra::Client