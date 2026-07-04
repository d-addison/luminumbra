// GPU-03 + GPU-P05 (spec 021 rank 66) leg B implementation: render the calibration
// cube through Diligent's GL backend. This is the FIRST render-through-Diligent code
// in the project (P02 only created + released a device). It is the only render TU
// that includes Diligent headers -- no Diligent type escapes (see dual_backend_diligent.h).
//
// Fidelity discipline: the GLSL below is math-identical to res/shaders/basic.{vert,frag}
// (the raw-GL golden's shader), differing ONLY in that the uniforms live in a std140
// UBO ("Constants") because Diligent binds constant buffers through its SRB, not the
// GL default uniform block. normalMatrix is identity, so `mat3(normalMatrix)*aNormal`
// == `aNormal`; every other term is a direct transcription. Any non-zero FLIP vs the
// golden is therefore attributable to the BACKEND (or a Y/gamma convention), which is
// exactly what leg B measures.

#include "dual_backend_diligent.h"

#include "EngineFactoryOpenGL.h"
#include "RenderDevice.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "Shader.h"
#include "Buffer.h"
#include "Texture.h"
#include "TextureView.h"
#include "ShaderResourceBinding.h"
#include "GraphicsTypes.h"
#include "RefCntAutoPtr.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace luminumbra_test {

namespace {

using namespace Diligent;

// std140 layout mirror of the "Constants" UBO. 4 mat4 (64B each) + 4 vec4 (16B each)
// = 320 bytes, every member naturally 16-aligned -> no packing surprises.
struct Constants {
    float model[16];
    float view[16];
    float projection[16];
    float normalMatrix[16];
    float lightPos[4];
    float viewPos[4];
    float lightColor[4];
    float objectColor[4];
};

const char* const kVertGlsl = R"GLSL(#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(std140) uniform Constants {
    mat4 g_model;
    mat4 g_view;
    mat4 g_projection;
    mat4 g_normalMatrix;
    vec4 g_lightPos;
    vec4 g_viewPos;
    vec4 g_lightColor;
    vec4 g_objectColor;
};
out vec3 FragPos;
out vec3 Normal;
void main() {
    FragPos = vec3(g_model * vec4(aPos, 1.0));
    Normal = mat3(g_normalMatrix) * aNormal;
    gl_Position = g_projection * g_view * vec4(FragPos, 1.0);
}
)GLSL";

const char* const kFragGlsl = R"GLSL(#version 450
in vec3 FragPos;
in vec3 Normal;
layout(std140) uniform Constants {
    mat4 g_model;
    mat4 g_view;
    mat4 g_projection;
    mat4 g_normalMatrix;
    vec4 g_lightPos;
    vec4 g_viewPos;
    vec4 g_lightColor;
    vec4 g_objectColor;
};
out vec4 FragColor;
void main() {
    float ambientStrength = 0.2;
    vec3 ambient = ambientStrength * g_lightColor.rgb;
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(g_lightPos.rgb - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * g_lightColor.rgb;
    float specularStrength = 0.5;
    vec3 viewDir = normalize(g_viewPos.rgb - FragPos);
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(norm, halfwayDir), 0.0), 32.0);
    vec3 specular = specularStrength * spec * g_lightColor.rgb;
    vec3 result = (ambient + diffuse + specular) * g_objectColor.rgb;
    FragColor = vec4(result, 1.0);
}
)GLSL";

void FillVec4(float* dst, const glm::vec3& v, float w) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

}  // namespace

DiligentRenderResult RenderCubeDiligentGl(const std::vector<MeshVertex>& mesh,
                                          const RenderParams& params) {
    DiligentRenderResult out;

    // --- Device: attach to the GL context current on this thread (caller owns it). ---
    IEngineFactoryOpenGL* factory = GetEngineFactoryOpenGL();
    if (factory == nullptr) {
        out.diagnostic = "GetEngineFactoryOpenGL returned null";
        return out;
    }
    EngineGLCreateInfo create_info;
    RefCntAutoPtr<IRenderDevice> device;
    RefCntAutoPtr<IDeviceContext> context;
    factory->AttachToActiveGLContext(create_info, &device, &context);
    if (!device || !context) {
        out.diagnostic = "AttachToActiveGLContext produced a null device/context";
        return out;
    }

    // --- Shaders (GLSL verbatim: used as-is, no HLSL cross-compile in this leg). ---
    auto make_shader = [&](SHADER_TYPE type, const char* src, const char* name,
                           RefCntAutoPtr<IShader>& shader) -> bool {
        ShaderCreateInfo sci;
        sci.SourceLanguage = SHADER_SOURCE_LANGUAGE_GLSL_VERBATIM;
        sci.Desc.ShaderType = type;
        sci.Desc.Name = name;
        sci.Source = src;
        sci.EntryPoint = "main";
        RefCntAutoPtr<IDataBlob> compiler_output;
        device->CreateShader(sci, &shader, &compiler_output);
        if (!shader) {
            out.diagnostic = std::string("CreateShader failed for ") + name;
            if (compiler_output && compiler_output->GetSize() > 0) {
                out.diagnostic += ": ";
                out.diagnostic += static_cast<const char*>(compiler_output->GetConstDataPtr());
            }
            return false;
        }
        return true;
    };
    RefCntAutoPtr<IShader> vs, ps;
    if (!make_shader(SHADER_TYPE_VERTEX, kVertGlsl, "cube VS", vs)) return out;
    if (!make_shader(SHADER_TYPE_PIXEL, kFragGlsl, "cube PS", ps)) return out;

    // --- Pipeline state: 1x RGBA8 RT + D32 depth, no cull, depth-test LESS. ---
    GraphicsPipelineStateCreateInfo pso_ci;
    pso_ci.PSODesc.Name = "cube PSO";
    GraphicsPipelineDesc& gp = pso_ci.GraphicsPipeline;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = TEX_FORMAT_RGBA8_UNORM;   // NON-sRGB: match raw-GL byte quantisation
    gp.DSVFormat = TEX_FORMAT_D32_FLOAT;
    gp.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    gp.RasterizerDesc.CullMode = CULL_MODE_NONE;            // raw-GL disables face cull
    gp.DepthStencilDesc.DepthEnable = True;
    gp.DepthStencilDesc.DepthWriteEnable = True;
    gp.DepthStencilDesc.DepthFunc = COMPARISON_FUNC_LESS;   // GL default depth func

    LayoutElement layout_elems[] = {
        LayoutElement{0, 0, 3, VT_FLOAT32, False},   // aPos
        LayoutElement{1, 0, 3, VT_FLOAT32, False},   // aNormal
    };
    gp.InputLayout.LayoutElements = layout_elems;
    gp.InputLayout.NumElements = 2;

    pso_ci.pVS = vs;
    pso_ci.pPS = ps;
    // Default resource-variable type STATIC: bind the single UBO on the PSO directly.

    RefCntAutoPtr<IPipelineState> pso;
    device->CreateGraphicsPipelineState(pso_ci, &pso);
    if (!pso) {
        out.diagnostic = "CreateGraphicsPipelineState failed";
        return out;
    }

    // --- Constant buffer (immutable; one draw, fixed transforms/light). ---
    Constants c{};
    const glm::mat4 model(1.0f);
    const glm::mat4 view = glm::lookAt(params.camera_position, params.camera_target,
                                       glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f), static_cast<float>(kCubeWidth) / kCubeHeight, 0.1f, 128.0f);
    const glm::mat4 normal_matrix(1.0f);
    std::memcpy(c.model, glm::value_ptr(model), sizeof(c.model));
    std::memcpy(c.view, glm::value_ptr(view), sizeof(c.view));
    std::memcpy(c.projection, glm::value_ptr(projection), sizeof(c.projection));
    std::memcpy(c.normalMatrix, glm::value_ptr(normal_matrix), sizeof(c.normalMatrix));
    FillVec4(c.lightPos, params.light_pos, 0.0f);
    FillVec4(c.viewPos, params.camera_position, 0.0f);
    FillVec4(c.lightColor, glm::vec3{1.0f, 1.0f, 1.0f}, 0.0f);
    FillVec4(c.objectColor, params.object_color, 0.0f);

    BufferDesc cb_desc;
    cb_desc.Name = "Constants";
    cb_desc.Usage = USAGE_IMMUTABLE;
    cb_desc.BindFlags = BIND_UNIFORM_BUFFER;
    cb_desc.Size = sizeof(Constants);
    BufferData cb_data;
    cb_data.pData = &c;
    cb_data.DataSize = sizeof(Constants);
    RefCntAutoPtr<IBuffer> cbuffer;
    device->CreateBuffer(cb_desc, &cb_data, &cbuffer);
    if (!cbuffer) {
        out.diagnostic = "CreateBuffer (Constants) failed";
        return out;
    }

    // Bind the UBO as a static variable on whichever stages reference it.
    bool bound = false;
    for (SHADER_TYPE st : {SHADER_TYPE_VERTEX, SHADER_TYPE_PIXEL}) {
        if (IShaderResourceVariable* var = pso->GetStaticVariableByName(st, "Constants")) {
            var->Set(cbuffer);
            bound = true;
        }
    }
    if (!bound) {
        out.diagnostic = "PSO exposes no 'Constants' UBO variable to bind";
        return out;
    }

    RefCntAutoPtr<IShaderResourceBinding> srb;
    pso->CreateShaderResourceBinding(&srb, true);
    if (!srb) {
        out.diagnostic = "CreateShaderResourceBinding failed";
        return out;
    }

    // --- Vertex buffer (immutable). ---
    BufferDesc vb_desc;
    vb_desc.Name = "cube VB";
    vb_desc.Usage = USAGE_IMMUTABLE;
    vb_desc.BindFlags = BIND_VERTEX_BUFFER;
    vb_desc.Size = mesh.size() * sizeof(MeshVertex);
    BufferData vb_data;
    vb_data.pData = mesh.data();
    vb_data.DataSize = vb_desc.Size;
    RefCntAutoPtr<IBuffer> vbo;
    device->CreateBuffer(vb_desc, &vb_data, &vbo);
    if (!vbo) {
        out.diagnostic = "CreateBuffer (vertex) failed";
        return out;
    }

    // --- Render target + depth textures. ---
    TextureDesc rt_desc;
    rt_desc.Name = "cube color RT";
    rt_desc.Type = RESOURCE_DIM_TEX_2D;
    rt_desc.Width = kCubeWidth;
    rt_desc.Height = kCubeHeight;
    rt_desc.Format = TEX_FORMAT_RGBA8_UNORM;
    rt_desc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    rt_desc.Usage = USAGE_DEFAULT;
    RefCntAutoPtr<ITexture> color_tex;
    device->CreateTexture(rt_desc, nullptr, &color_tex);

    TextureDesc ds_desc = rt_desc;
    ds_desc.Name = "cube depth";
    ds_desc.Format = TEX_FORMAT_D32_FLOAT;
    ds_desc.BindFlags = BIND_DEPTH_STENCIL;
    RefCntAutoPtr<ITexture> depth_tex;
    device->CreateTexture(ds_desc, nullptr, &depth_tex);
    if (!color_tex || !depth_tex) {
        out.diagnostic = "CreateTexture (RT/depth) failed";
        return out;
    }

    ITextureView* rtv = color_tex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    ITextureView* dsv = depth_tex->GetDefaultView(TEXTURE_VIEW_DEPTH_STENCIL);

    // --- Draw. ---
    ITextureView* rtvs[] = {rtv};
    context->SetRenderTargets(1, rtvs, dsv, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clear_color[4] = {kClearR, kClearG, kClearB, 1.0f};
    context->ClearRenderTarget(rtv, clear_color, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->ClearDepthStencil(dsv, CLEAR_DEPTH_FLAG, 1.0f, 0,
                               RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    context->SetPipelineState(pso);
    context->CommitShaderResources(srb, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    IBuffer* vbs[] = {vbo};
    const Uint64 offsets[] = {0};
    context->SetVertexBuffers(0, 1, vbs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                              SET_VERTEX_BUFFERS_FLAG_RESET);

    DrawAttribs draw;
    draw.NumVertices = static_cast<Uint32>(mesh.size());
    draw.Flags = DRAW_FLAG_VERIFY_ALL;
    context->Draw(draw);

    // --- Readback via staging texture (GL: CopyTexture -> glReadPixels into PBO). ---
    TextureDesc stg_desc = rt_desc;
    stg_desc.Name = "cube staging";
    stg_desc.BindFlags = BIND_NONE;
    stg_desc.Usage = USAGE_STAGING;
    stg_desc.CPUAccessFlags = CPU_ACCESS_READ;
    RefCntAutoPtr<ITexture> staging;
    device->CreateTexture(stg_desc, nullptr, &staging);
    if (!staging) {
        out.diagnostic = "CreateTexture (staging) failed";
        return out;
    }

    CopyTextureAttribs copy;
    copy.pSrcTexture = color_tex;
    copy.SrcTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = staging;
    copy.DstTextureTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture(copy);

    context->WaitForIdle();  // ensure the glReadPixels-into-PBO has completed

    MappedTextureSubresource mapped;
    context->MapTextureSubresource(staging, 0, 0, MAP_READ, MAP_FLAG_NONE, nullptr, mapped);
    if (mapped.pData == nullptr) {
        out.diagnostic = "MapTextureSubresource returned null";
        return out;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(kCubeWidth) * 4u;
    out.pixels.resize(static_cast<std::size_t>(kCubeWidth) * kCubeHeight * 4u);
    const auto* src = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < kCubeHeight; ++y) {
        std::memcpy(out.pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                    src + static_cast<std::size_t>(y) * mapped.Stride, row_bytes);
    }
    context->UnmapTextureSubresource(staging, 0, 0);

    out.available = true;
    return out;
}

}  // namespace luminumbra_test
