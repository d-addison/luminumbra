#pragma once

#include "../RenderContext.h"
#include "../RenderInputs.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Shader;

struct GlassOitPassInput {
    const std::vector<GlassPaneItem>* glass_items = nullptr;
    u32 glass_vao = 0;
    std::filesystem::path root_path;
};

// Weighted blended order-independent-transparency glass pass extracted from
// RenderPipeline. Accumulation and resolve stay together because they share the
// two accumulation targets, MRT FBO, and shaders.
//
// Converted to the RenderContext seam: accumulation reads the prepared view and
// projection, camera, output/internal extents, opaque-scene color, and shared
// lighting depth from ctx; resolve reads the lit-scene FBO, internal extent, and
// fullscreen quad from ctx. The pane list, pane VAO, and shader root are
// pass-specific inputs. The MRT FBO and textures remain pass-owned raw GL
// objects, preserving their lazy allocation and cleanup lifetime.
class GlassOitPass {
public:
    GlassOitPass();
    ~GlassOitPass();

    void execute_accum(const RenderContext& ctx, const GlassOitPassInput& input);
    void execute_resolve(const RenderContext& ctx, const GlassOitPassInput& input);
    void destroy();

    const std::unique_ptr<Shader>& accum_shader() const {
        return m_accum_shader;
    }
    const std::unique_ptr<Shader>& resolve_shader() const {
        return m_resolve_shader;
    }

private:
    std::unique_ptr<Shader> m_accum_shader;
    std::unique_ptr<Shader> m_resolve_shader;
    u32 m_fbo = 0;
    u32 m_accum_tex = 0;  // RGBA16F: rgb = sum(w*a*c), a = sum(w*a)
    u32 m_reveal_tex = 0; // R16F: product of (1 - a_i)
};

} // namespace Luminumbra::Rendering
