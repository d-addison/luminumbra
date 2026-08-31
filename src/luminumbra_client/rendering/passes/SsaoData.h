#pragma once

//  (, struct extraction): SSAOData moved out of RenderPipeline.h so
// SsaoPass can own it without including the pipeline god-object. RenderPipeline.h
// still includes this header (it reads ssao resources for the inventory/VRAM
// stats), so the move is behavior-neutral. Self-contained per the verified-T02
// lesson: an extracted struct header must carry its own includes + fwd-decls.

#include "../../include/luminumbra/core/Types.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Shader;

struct SSAOData {
    GLuint fbo = 0, blurFBO = 0;
    GLuint ssaoColorBuffer = 0, ssaoColorBufferBlur = 0;
    GLuint noiseTexture = 0;
    std::vector<glm::vec3> kernel;
    std::unique_ptr<Shader> ssaoShader;
    std::unique_ptr<Shader> blurShader;
    // Render-optimization (ssao-gtao): XeGTAO horizon-slice AO, selected when
    // ssao_quality > 0. Renders into the same FBOs as the legacy SSAO so the blur
    // + lighting AO tap are unchanged. Null/quality 0 -> legacy ssaoShader path.
    std::unique_ptr<Shader> gtaoShader;
    // ssao_quality 3 = HALF-RES GTAO: render GTAO into a 1/2-per-axis FBO (1/4 the
    // fragments) then joint-bilateral depth-aware upsample to full res (this replaces
    // the box blur on that path). The AO budget holds even on dense views.
    GLuint halfFBO = 0;
    GLuint halfTex = 0; // half-res AO (R16F)
    u32 halfW = 0, halfH = 0;
    std::unique_ptr<Shader> upsampleShader;
};

} // namespace Luminumbra::Rendering
