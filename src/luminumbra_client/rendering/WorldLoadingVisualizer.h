#pragma once

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include <filesystem>
#include <glad/glad.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Client {

enum class ChunkLoadVisualState {
    UNDEFINED,
    DISPATCHED
};

struct ChunkInstanceData {
    Mat4 transform;
    Vec4 color;
};

class WorldLoadingVisualizer {
public:
    WorldLoadingVisualizer();
    ~WorldLoadingVisualizer();

    void Startup(const std::filesystem::path& root_path, int screen_width, int screen_height);
    void Shutdown();

    void BeginVisualization(const std::vector<IVec3>& initial_chunks);
    void UpdateChunkState(const IVec3& chunk_coords, ChunkLoadVisualState state);
    void EndVisualization();

    void UpdateAndRender(float delta_time, const std::string& status_text, float progress);

private:
    std::unique_ptr<Rendering::Shader> m_shader;
    std::unique_ptr<Rendering::Camera> m_camera;

    // Cube mesh
    GLuint m_cube_vao = 0;
    GLuint m_cube_vbo = 0;
    GLuint m_cube_ebo = 0;

    // Instancing
    GLuint m_instance_vbo = 0;
    std::vector<ChunkInstanceData> m_instance_data;
    std::unordered_map<ChunkID, size_t> m_chunk_to_instance_map;
    bool m_instance_buffer_dirty = false;

    // Camera animation
    float m_camera_angle = 0.0f;
    float m_camera_distance = 250.0f;

    bool m_is_active = false;
};

} // namespace Luminumbra::Client
