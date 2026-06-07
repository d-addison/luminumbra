#include "debug/WorldGenViewer.h"
#include <imgui.h>
#include <glm/glm.hpp>
#include <string>
#include "core/Log.h"

namespace Luminumbra::Client {

WorldGenViewer::WorldGenViewer() {
    // Start with default params. In a real app, these might come from the main world system.
    m_params = Systems::TerrainGenParams();
    snprintf(m_seed_buf, sizeof(m_seed_buf), "%d", m_seed);

    // Prepare pixel buffer and OpenGL texture for the preview
    m_pixelBuffer.resize(m_textureSize * m_textureSize * 3);
    glGenTextures(1, &m_textureID);
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_textureSize, m_textureSize, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    // Create the initial world system instance for the viewer
    RecreateWorldSystem();
}

WorldGenViewer::~WorldGenViewer() {
    if (m_textureID) {
        glDeleteTextures(1, &m_textureID);
    }
}

void WorldGenViewer::RecreateWorldSystem() {
    m_viewerWorldSystem = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, nullptr, m_params, m_seed);
}

void WorldGenViewer::UpdateAndRender(bool& is_open, Systems::SHIELD_WorldSystem* main_world_system) {
    if (!is_open) return;

    // Re-create the world system if parameters have changed, then regenerate the texture.
    if (m_paramsChanged) {
        RecreateWorldSystem();
        RegenerateTexture();
        m_paramsChanged = false;
    }

    ImGui::SetNextWindowSize(ImVec2(550, 750), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("SHIELD Generator Inspector", &is_open)) {
        ImGui::End();
        return;
    }

    // --- Preview Section ---
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0,0));
    ImGui::Image((void*)(intptr_t)m_textureID, ImVec2(m_textureSize, m_textureSize));
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("SDF Slice Preview\nSolid (Green), Air (Blue), Surface (White)");
    }
    ImGui::Text("Slice at Y: %.1f, Zoom: %.2fx", m_sliceY, m_zoom);
    if (ImGui::SliderFloat("Y Slice Level", &m_sliceY, -128.0f, 256.0f)) RegenerateTexture();
    if (ImGui::SliderFloat("Zoom", &m_zoom, 0.05f, 5.0f)) RegenerateTexture();


    // --- Parameter Section ---
    if (ImGui::CollapsingHeader("Generator Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText("Seed", m_seed_buf, sizeof(m_seed_buf), ImGuiInputTextFlags_CharsDecimal)) {
            m_seed = std::atoi(m_seed_buf);
            m_paramsChanged = true;
        }

        if (ImGui::Button("Apply to World & Regenerate")) {
             if (main_world_system) {
                 LUMINUMBRA_CORE_INFO("Applying new generator settings to the world.");
                 main_world_system->set_params(m_params);
                 main_world_system->set_seed(m_seed);
                 main_world_system->regenerate_all_chunks(nullptr); // Pass physics system if needed
             }
        }
    }


    if (ImGui::CollapsingHeader("Terrain Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Frequency", &m_params.base_frequency, 0.0001f, 0.05f, "%.4f")) m_paramsChanged = true;
        if (ImGui::SliderFloat("Amplitude", &m_params.base_amplitude, 0.0f, 250.0f)) m_paramsChanged = true;
        if (ImGui::SliderInt("Octaves", &m_params.octaves, 1, 10)) m_paramsChanged = true;
        if (ImGui::SliderFloat("Persistence", &m_params.persistence, 0.1f, 1.0f)) m_paramsChanged = true;
        if (ImGui::SliderFloat("Lacunarity", &m_params.lacunarity, 1.5f, 4.0f)) m_paramsChanged = true;
        if (ImGui::SliderFloat("Height Offset", &m_params.height_offset, -128.0f, 128.0f)) m_paramsChanged = true;
    }

    if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Caves", &m_params.caves_enabled)) m_paramsChanged = true;
        if (m_params.caves_enabled) {
            if (ImGui::SliderFloat("Cave Frequency", &m_params.cave_frequency, 0.005f, 0.1f, "%.3f")) m_paramsChanged = true;
        }

        ImGui::Separator();

        if (ImGui::Checkbox("Island Mask", &m_params.island_mask_enabled)) m_paramsChanged = true;
        if (m_params.island_mask_enabled) {
            if (ImGui::SliderFloat("Island Mask Freq.", &m_params.island_mask_frequency, 0.0001f, 0.01f, "%.4f")) m_paramsChanged = true;
        }
    }

    ImGui::End();
}

void WorldGenViewer::RegenerateTexture() {
    if (!m_viewerWorldSystem) return;

    for (int y = 0; y < m_textureSize; ++y) {
        for (int x = 0; x < m_textureSize; ++x) {
            // Map pixel coordinates to world coordinates for sampling
            float worldX = (x - m_textureSize / 2.0f) * m_zoom + m_offsetX;
            float worldZ = (y - m_textureSize / 2.0f) * m_zoom + m_offsetZ;
            Vec3 world_pos(worldX, m_sliceY, worldZ);

            // Get the density value from our internal world generator
            float density = m_viewerWorldSystem->get_density_at(world_pos);

            // Color the pixel based on density
            uint8_t r = 0, g = 0, b = 0;
            if (density <= 0) { // Solid ground
                g = static_cast<uint8_t>(glm::clamp(150.0 + density * 5.0, 0.0, 255.0));
            } else { // Air
                b = static_cast<uint8_t>(glm::clamp(50.0 + density * 10.0, 0.0, 255.0));
            }
            // Highlight the zero-crossing (the surface) in white for clarity
            if (std::abs(density) < 0.5f) {
                r = 255; g = 255; b = 255;
            }

            // Write to the pixel buffer
            int index = (y * m_textureSize + x) * 3;
            m_pixelBuffer[index] = r;
            m_pixelBuffer[index + 1] = g;
            m_pixelBuffer[index + 2] = b;
        }
    }

    // Upload the new pixel data to the GPU texture
    glBindTexture(GL_TEXTURE_2D, m_textureID);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_textureSize, m_textureSize, GL_RGB, GL_UNSIGNED_BYTE, m_pixelBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace Luminumbra::Client