#pragma once

#include "../RenderContext.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Shader;

// Screen-space crepuscular-rays fullscreen pass extracted from RenderPipeline.
// Owns only its shader; all frame state comes from the RenderContext seam.
class GodRaysPass {
public:
    GodRaysPass();
    ~GodRaysPass();

    void init_shader(const std::filesystem::path& root_path);
    void reset_shader();
    void execute(const RenderContext& ctx);

    const std::unique_ptr<Shader>& shader() const {
        return m_shader;
    }

private:
    std::unique_ptr<Shader> m_shader;
};

} // namespace Luminumbra::Rendering
