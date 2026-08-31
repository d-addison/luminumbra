#include "debug/WorldGenViewer.h"
#include "core/Log.h"
#include "luminumbra_common/world/LayerGraph.h"
#include <glm/glm.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Luminumbra::Client {

const char* WorldGenViewer::LayerArtifactSchema() {
    return "luminumbra.worldgen_layers.v1";
}

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
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGB,
                 m_textureSize,
                 m_textureSize,
                 0,
                 GL_RGB,
                 GL_UNSIGNED_BYTE,
                 nullptr);
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
    m_viewerWorldSystem =
        std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, nullptr, m_params, m_seed);
}

void WorldGenViewer::UpdateAndRender(bool& is_open,
                                     Systems::SHIELD_WorldSystem* main_world_system) {
    if (!is_open)
        return;

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
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::Image((void*)(intptr_t)m_textureID, ImVec2(m_textureSize, m_textureSize));
    ImGui::PopStyleVar();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("SDF Slice Preview\nSolid (Green), Air (Blue), Surface (White)");
    }
    ImGui::Text("Slice at Y: %.1f, Zoom: %.2fx", m_sliceY, m_zoom);
    if (ImGui::SliderFloat("Y Slice Level", &m_sliceY, -128.0f, 256.0f))
        RegenerateTexture();
    if (ImGui::SliderFloat("Zoom", &m_zoom, 0.05f, 5.0f))
        RegenerateTexture();

    // --- Parameter Section ---
    if (ImGui::CollapsingHeader("Generator Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::InputText(
                "Seed", m_seed_buf, sizeof(m_seed_buf), ImGuiInputTextFlags_CharsDecimal)) {
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

    if (ImGui::CollapsingHeader("Artifact Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Schema: %s", LayerArtifactSchema());
        if (ImGui::Checkbox("01 Base Terrain", &m_showBaseTerrainLayer))
            m_paramsChanged = true;
        if (ImGui::Checkbox("02 Island Mask", &m_showIslandMaskLayer))
            m_paramsChanged = true;
        if (ImGui::Checkbox("03 Caves", &m_showCaveLayer))
            m_paramsChanged = true;
        if (ImGui::Checkbox("05 Submerged Water", &m_showWaterLayer))
            m_paramsChanged = true;
        ImGui::Checkbox("Topology Deltas", &m_showTopologyDeltas);
        ImGui::Text("Delta signals: changed SDF samples, SDF sign flips, height deltas");
    }

    if (ImGui::CollapsingHeader("Terrain Shape", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Frequency", &m_params.base_frequency, 0.0001f, 0.05f, "%.4f"))
            m_paramsChanged = true;
        if (ImGui::SliderFloat("Amplitude", &m_params.base_amplitude, 0.0f, 250.0f))
            m_paramsChanged = true;
        if (ImGui::SliderInt("Octaves", &m_params.octaves, 1, 10))
            m_paramsChanged = true;
        if (ImGui::SliderFloat("Persistence", &m_params.persistence, 0.1f, 1.0f))
            m_paramsChanged = true;
        if (ImGui::SliderFloat("Lacunarity", &m_params.lacunarity, 1.5f, 4.0f))
            m_paramsChanged = true;
        if (ImGui::SliderFloat("Height Offset", &m_params.height_offset, -128.0f, 128.0f))
            m_paramsChanged = true;
    }

    if (ImGui::CollapsingHeader("Features", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Caves", &m_params.caves_enabled))
            m_paramsChanged = true;
        if (m_params.caves_enabled) {
            if (ImGui::SliderFloat(
                    "Cave Frequency", &m_params.cave_frequency, 0.005f, 0.1f, "%.3f"))
                m_paramsChanged = true;
        }

        ImGui::Separator();

        if (ImGui::Checkbox("Island Mask", &m_params.island_mask_enabled))
            m_paramsChanged = true;
        if (m_params.island_mask_enabled) {
            if (ImGui::SliderFloat(
                    "Island Mask Freq.", &m_params.island_mask_frequency, 0.0001f, 0.01f, "%.4f"))
                m_paramsChanged = true;
        }
    }

    //  the constrained layer-graph authoring panel, behind the
    // --worldgen-graph flag (m_graphEnabled). Renders the fixed pipeline stages as
    // ImGui cards (edges implicit/fixed in v1) over a preset JSON derived from the
    // viewer's current params. Read-only view in v1.
    if (m_graphEnabled) {
        RenderLayerGraphPanel();
    }

    ImGui::End();
}

void WorldGenViewer::RenderLayerGraphPanel() {
    if (!ImGui::CollapsingHeader("Layer Graph (constrained)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    using namespace Luminumbra::world;

    // Build a preset JSON from the viewer's current scalar params (the subset the
    // inspector exposes). This mirrors the shipped generation_params schema so the
    // decomposition is the SAME fixed topology the create-world graph authors. The
    // engine LayerGraph guarantees Compile(graph) is bit-exact to this JSON.
    nlohmann::json preset;
    nlohmann::json& gp = preset["generation_params"];
    gp["terrain"] = {
        {"base_frequency", m_params.base_frequency},
        {"base_amplitude", m_params.base_amplitude},
        {"octaves", m_params.octaves},
        {"persistence", m_params.persistence},
        {"lacunarity", m_params.lacunarity},
        {"height_offset", m_params.height_offset},
        {"island_mask_enabled", m_params.island_mask_enabled},
    };
    gp["features"] = {
        {"caves_enabled", m_params.caves_enabled},
        {"cave_frequency", m_params.cave_frequency},
    };

    const LayerGraph graph = LayerGraphFromPreset(preset);

    ImGui::TextDisabled("Schema: %s  |  fixed topology, edges implicit", LayerGraphSchema());
    ImGui::Text("Pipeline order (base FBM ->... -> materials):");

    // One ImGui "card" per stage, drawn top-to-bottom in the fixed edge order.
    bool first = true;
    for (const LayerNode& n : graph.nodes) {
        if (!first) {
            // A fixed downward edge marker between consecutive stage cards.
            ImGui::Indent(12.0f);
            ImGui::TextDisabled("|");
            ImGui::TextDisabled("v");
            ImGui::Unindent(12.0f);
        }
        first = false;

        ImGui::PushID(LayerStageId(n.stage));
        ImGui::BeginChild(LayerStageId(n.stage),
                          ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Border);
        ImGui::TextColored(n.present ? ImVec4(0.7f, 0.9f, 1.0f, 1.0f)
                                     : ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "%s",
                           LayerStageLabel(n.stage));
        if (!n.present) {
            ImGui::TextDisabled("  (inactive in this preset)");
        } else if (n.params.is_object()) {
            for (auto it = n.params.begin(); it != n.params.end(); ++it) {
                // Compact one-line readout of each scalar in the stage slice.
                ImGui::BulletText("%s = %s", it.key().c_str(), it.value().dump().c_str());
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }

    // The bit-exact round-trip the determinism test pins, surfaced live so the
    // authoring tool proves Compile(graph) == the source params at a glance.
    const nlohmann::json compiled = CompileLayerGraph(graph);
    const bool bit_exact = (compiled == gp);
    ImGui::Separator();
    ImGui::TextColored(bit_exact ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                       "compile(graph) %s bit-exact to generation_params",
                       bit_exact ? "==" : "!=");
}

void WorldGenViewer::RegenerateTexture() {
    if (!m_viewerWorldSystem)
        return;

    for (int y = 0; y < m_textureSize; ++y) {
        for (int x = 0; x < m_textureSize; ++x) {
            // Map pixel coordinates to world coordinates for sampling
            float worldX = (x - m_textureSize / 2.0f) * m_zoom + m_offsetX;
            float worldZ = (y - m_textureSize / 2.0f) * m_zoom + m_offsetZ;
            Vec3 world_pos(worldX, m_sliceY, worldZ);

            const Systems::WorldGenLayerSample sample =
                m_viewerWorldSystem->SampleWorldGenLayers(world_pos);
            const float density = sample.final_density;

            // Color the pixel based on density
            uint8_t r = 0, g = 0, b = 0;
            if (density <= 0) { // Solid ground
                g = static_cast<uint8_t>(glm::clamp(150.0 + density * 5.0, 0.0, 255.0));
            } else { // Air
                b = static_cast<uint8_t>(glm::clamp(50.0 + density * 10.0, 0.0, 255.0));
            }
            // Highlight the zero-crossing (the surface) in white for clarity
            if (std::abs(density) < 0.5f) {
                r = 255;
                g = 255;
                b = 255;
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
    glTexSubImage2D(GL_TEXTURE_2D,
                    0,
                    0,
                    0,
                    m_textureSize,
                    m_textureSize,
                    GL_RGB,
                    GL_UNSIGNED_BYTE,
                    m_pixelBuffer.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

} // namespace Luminumbra::Client
