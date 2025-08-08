#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <glad/glad.h>
#include <vector>
#include <memory>
#include <unordered_map>

namespace Luminumbra::Systems { class SHIELD_WorldSystem; }

namespace Luminumbra::Rendering {

class Shader;
class Camera;

struct GBuffer {
    u32 fbo_id = 0;
    u32 position_texture = 0;
    u32 normal_texture = 0;
    u32 albedo_texture = 0;
    u32 depth_texture = 0; // actually a renderbuffer in current impl
};

struct ChunkRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
};

class RenderPipeline {
public:
    RenderPipeline();
    ~RenderPipeline(); // NEW

    void startup(u32 screen_width, u32 screen_height, const std::string& root_path);
    void render_frame(Systems::SHIELD_WorldSystem& world_system, const Camera& camera);
    void on_resize(u32 new_width, u32 new_height);
    void unload_chunk_resources(ChunkID chunk_id);

private:
    void geometry_pass(Systems::SHIELD_WorldSystem& world_system, const Camera& camera);
    void lighting_pass(const Camera& camera);
    void far_field_pass(const Camera& camera);
    void composite_pass();
    void skybox_pass(const Camera& camera);

    void init_gbuffer(u32 width, u32 height);
    void destroy_gbuffer(); // NEW
    void init_shaders();
    void init_screen_quad();
    void cleanup_gpu_resources();

    u32 m_screen_width = 0;
    u32 m_screen_height = 0;

    std::unique_ptr<Shader> m_geometry_shader;
    std::unique_ptr<Shader> m_lighting_shader;
    std::unique_ptr<Shader> m_far_field_shader;
    std::unique_ptr<Shader> m_composite_shader;
    std::unique_ptr<Shader> m_skybox_shader;

    std::string m_root_path;

    GBuffer m_gbuffer;
    u32 m_far_field_fbo = 0;
    u32 m_far_field_texture = 0;

    std::unordered_map<ChunkID, ChunkRenderData> m_chunk_render_data;

    u32 m_screen_quad_vao = 0;
    u32 m_screen_quad_vbo = 0; // NEW
};

} // namespace Luminumbra::Rendering