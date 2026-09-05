#pragma once

#include "../RenderContext.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Shader;

// Analytic aerial-perspective fullscreen pass extracted from RenderPipeline.
// Owns its shader; frame resources and atmosphere state arrive through the
// RenderContext seam. The froxel volume remains pipeline-owned and is supplied
// as the pass input immediately before execution.
class AerialPass {
public:
    AerialPass();
    ~AerialPass();

    void init_shader(const std::filesystem::path& root_path);
    void reset_shader();
    void set_froxel_input(int volumetric_quality, u32 integrated_texture);
    void execute(const RenderContext& ctx);

    const std::unique_ptr<Shader>& shader() const {
        return m_shader;
    }

private:
    std::unique_ptr<Shader> m_shader;
    int m_volumetric_quality = 0;
    u32 m_froxel_integrated_texture = 0;
};

} // namespace Luminumbra::Rendering
