#pragma once

#include "../RenderContext.h"
#include "SsaoData.h"

#include <filesystem>
#include <memory>

namespace Luminumbra::Rendering {

class Camera;
class Shader;
class RenderResourceRegistry;

// SSAO + SSAO blur render passes extracted from RenderPipeline.
// Owns the SSAO FBOs, color buffers, noise texture, sample kernel, and both
// SSAO shaders (SSAOData).: converted to the RenderContext
// seam — execute takes a const RenderContext& (G-buffer inputs, screen quad,
// quality, screen size from the contract), NOT a RenderPipeline&. The pipeline
// keeps orchestration order, stats collection, and the GPU timer issue/collect.
class SsaoPass {
public:
    SsaoPass();
    ~SsaoPass();

    void init_shaders(const std::filesystem::path& root_path);
    // the AO RENDER TARGETS (raw/blur full-res + half-res GTAO
    // FBOs and their color textures) are REGISTRY-OWNED. The 4x4 noise texture is
    // NOT — it carries uploaded pixel data (a static input, not a render target),
    // so it stays pass-owned. The SSAOData struct caches the owned ids so every
    // reader is unchanged.
    void init_ssao(RenderResourceRegistry& registry, u32 width, u32 height);
    void destroy_ssao(RenderResourceRegistry& registry);
    void reset_shaders();

    //  seam: source G-buffer/quality/quad/screen from the RenderContext.
    // The in-process A/B parity gate lives in RenderPipeline::capture_ssao_parity
    // (which has pipeline access): it runs the original pipeline-sourced GL
    // sequence as the golden A-leg and this ctx-sourced seam as the B-leg, then
    // memcmps the readback — catching any call-site ctx mis-population.
    void execute_ssao(const RenderContext& ctx);
    void execute_blur(const RenderContext& ctx);

    SSAOData& ssao() {
        return m_ssao;
    }
    const SSAOData& ssao() const {
        return m_ssao;
    }

private:
    SSAOData m_ssao;
};

} // namespace Luminumbra::Rendering
