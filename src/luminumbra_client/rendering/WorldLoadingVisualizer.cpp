#include "WorldLoadingVisualizer.h"
#include "core/Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>
#include <GLFW/glfw3.h>

extern std::vector<Luminumbra::IVec3> g_initial_chunks_to_load;
extern int g_generation_dispatch_index;

namespace Luminumbra::Client {

WorldLoadingVisualizer::WorldLoadingVisualizer() {}

WorldLoadingVisualizer::~WorldLoadingVisualizer() {}

void WorldLoadingVisualizer::Startup(const std::filesystem::path& root_path, int screen_width, int screen_height) {
    m_shader = std::make_unique<Rendering::Shader>(
        (root_path / "res/shaders/loading_hologram.vert").string().c_str(),
        (root_path / "res/shaders/loading_hologram.frag").string().c_str()
    );

    m_camera = std::make_unique<Rendering::Camera>(Vec3(0, 80, m_camera_distance));
    m_camera->Pitch = -20.0f;
    m_camera->updateCameraVectors();

    // 1. Create Wireframe Cube Mesh
    const float s = CHUNK_SIZE_X * 0.5f;
    const Vec3 vertices[] = {
        {-s, -s, -s}, {s, -s, -s}, {s,  s, -s}, {-s,  s, -s},
        {-s, -s,  s}, {s, -s,  s}, {s,  s,  s}, {-s,  s,  s}
    };
    const GLuint indices[] = {
        0, 1, 1, 2, 2, 3, 3, 0, // Bottom face
        4, 5, 5, 6, 6, 7, 7, 4, // Top face
        0, 4, 1, 5, 2, 6, 3, 7  // Vertical edges
    };

    glGenVertexArrays(1, &m_cube_vao);
    glGenBuffers(1, &m_cube_vbo);
    glGenBuffers(1, &m_cube_ebo);

    glBindVertexArray(m_cube_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_cube_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_cube_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vec3), (void*)0);
    
    // 2. Create and configure the VBO for instance data
    glGenBuffers(1, &m_instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_DYNAMIC_DRAW); 

    const GLuint MATRIX_ATTRIB_LOCATION = 1;
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(MATRIX_ATTRIB_LOCATION + i);
        glVertexAttribPointer(MATRIX_ATTRIB_LOCATION + i, 4, GL_FLOAT, GL_FALSE, sizeof(ChunkInstanceData), (void*)(offsetof(ChunkInstanceData, transform) + sizeof(Vec4) * i));
        glVertexAttribDivisor(MATRIX_ATTRIB_LOCATION + i, 1);
    }
    
    const GLuint COLOR_ATTRIB_LOCATION = 5; // 1 + 4 = 5
    glEnableVertexAttribArray(COLOR_ATTRIB_LOCATION);
    glVertexAttribPointer(COLOR_ATTRIB_LOCATION, 4, GL_FLOAT, GL_FALSE, sizeof(ChunkInstanceData), (void*)offsetof(ChunkInstanceData, color));
    glVertexAttribDivisor(COLOR_ATTRIB_LOCATION, 1);

    glBindVertexArray(0);
    LUMINUMBRA_CORE_INFO("World Loading Visualizer Initialized.");
}

void WorldLoadingVisualizer::Shutdown() {
    if (m_cube_vao) glDeleteVertexArrays(1, &m_cube_vao);
    if (m_cube_vbo) glDeleteBuffers(1, &m_cube_vbo);
    if (m_cube_ebo) glDeleteBuffers(1, &m_cube_ebo);
    if (m_instance_vbo) glDeleteBuffers(1, &m_instance_vbo);
    m_shader.reset();
}

void WorldLoadingVisualizer::BeginVisualization(const std::vector<IVec3>& initial_chunks) {
    m_is_active = true;
    m_instance_data.clear();
    m_chunk_to_instance_map.clear();
    m_instance_data.reserve(initial_chunks.size());

    for (const auto& coords : initial_chunks) {
        Vec3 center_pos = (Vec3(coords) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        Mat4 transform = glm::translate(Mat4(1.0f), center_pos);

        m_chunk_to_instance_map[Chunk::calculate_id(coords)] = m_instance_data.size();
        m_instance_data.push_back({transform, Vec4(0.1f, 0.1f, 0.1f, 0.0f)}); // Start dim and not pulsing
    }

    m_instance_buffer_dirty = true;
    LUMINUMBRA_CORE_INFO("Starting world loading visualization with {} chunks.", initial_chunks.size());
}

void WorldLoadingVisualizer::UpdateChunkState(const IVec3& chunk_coords, ChunkLoadVisualState state) {
    ChunkID id = Chunk::calculate_id(chunk_coords);
    if (m_chunk_to_instance_map.count(id)) {
        size_t index = m_chunk_to_instance_map[id];
        if (state == ChunkLoadVisualState::DISPATCHED) {
            // Bright cyan, pulsing
            m_instance_data[index].color = Vec4(0.2f, 0.8f, 1.0f, 1.0f); 
        }
        m_instance_buffer_dirty = true;
    }
}


void WorldLoadingVisualizer::EndVisualization() {
    m_is_active = false;
    LUMINUMBRA_CORE_INFO("Ending world loading visualization.");
}

void WorldLoadingVisualizer::UpdateAndRender(float delta_time, const std::string& status_text, float progress) {
    if (!m_is_active) return;

    // 1. Update Orbiting Camera
    m_camera_angle += 15.0f * delta_time; // degrees per second
    if (m_camera_angle > 360.0f) m_camera_angle -= 360.0f;
    float rad = glm::radians(m_camera_angle);
    m_camera->Position = Vec3(sin(rad) * m_camera_distance, 80.0f, cos(rad) * m_camera_distance);
    m_camera->Front = glm::normalize(Vec3(0, 40, 0) - m_camera->Position);
    m_camera->updateCameraVectors();

    // 2. Update Instance Buffer if needed
    if (m_instance_buffer_dirty) {
        glBindBuffer(GL_ARRAY_BUFFER, m_instance_vbo);
        glBufferData(GL_ARRAY_BUFFER, m_instance_data.size() * sizeof(ChunkInstanceData), m_instance_data.data(), GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_instance_buffer_dirty = false;
    }
    
    // 3. Render the scene
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glLineWidth(1.5f);

    m_shader->use();
    m_shader->setMat4("projection", m_camera->GetProjectionMatrix(1280, 720)); // Assume fixed aspect for loading
    m_shader->setMat4("view", m_camera->GetViewMatrix());
    m_shader->setFloat("u_time", static_cast<float>(glfwGetTime()));
    m_shader->setVec3("u_viewPos", m_camera->Position);

    glBindVertexArray(m_cube_vao);
    glDrawElementsInstanced(GL_LINES, 24, GL_UNSIGNED_INT, 0, m_instance_data.size());
    glBindVertexArray(0);

    glLineWidth(1.0f);
    glDisable(GL_BLEND);

    // 4. Render overlay UI using ImGui
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.85f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("Loading", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));
    ImGui::Text("%s", status_text.c_str());
    ImGui::ProgressBar(progress, ImVec2(500.0f, 20.0f));
    ImGui::Text("%d / %zu Chunks Dispatched", g_generation_dispatch_index, g_initial_chunks_to_load.size());
    ImGui::PopStyleVar();
    
    ImGui::End();
}

} // namespace Luminumbra::Client