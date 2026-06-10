#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// SSAO + SSAO blur render passes extracted from RenderPipeline (T-I2-11d).
// Owns the SSAO FBOs, color buffers, noise texture, sample kernel, and both
// SSAO shaders (SSAOData). The pipeline keeps orchestration order, the
// shared screen quad, the G-Buffer inputs, stats collection, and the GPU
// timer issue/collect calls.
class SsaoPass {
public:
    SsaoPass();
    ~SsaoPass();

    void init_shaders(const std::filesystem::path& root_path);
    void init_ssao(u32 width, u32 height);
    void destroy_ssao();
    void reset_shaders();

    void execute_ssao(RenderPipeline& pipeline, const Camera& camera);
    void execute_blur(RenderPipeline& pipeline);

    SSAOData& ssao() { return m_ssao; }
    const SSAOData& ssao() const { return m_ssao; }

private:
    SSAOData m_ssao;
};

} // namespace Luminumbra::Rendering
